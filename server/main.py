"""
Render server for the button -> camera -> vision -> haptic proof of concept.

Flow
  0. Camera board  WS   /camera/ws           -> stays connected; server pushes jobs down it
  1. Button board  POST /button              -> creates the single active job (409 if one exists,
                                                503 if the camera isn't connected) and sends
                                                {"cmd":"capture","job_id":...} to the camera
  3. Camera board  POST /camera/{id}/image/{rank}  (x4, raw JPEG body)
  4. Camera board  POST /camera/{id}/submit  -> server reads all 4 images in parallel, votes
  5. Button board  GET  /job/{id}            -> polls; when status == "done" it gets the result
                                                and the job is closed (button accepted again)

Only one job exists at a time. Run with ONE worker: state is in memory.
"""
import asyncio
import base64
import json
import logging
import os
import re
import time
import uuid
from collections import Counter

from anthropic import AsyncAnthropic
from fastapi import FastAPI, Header, HTTPException, Request, WebSocket, WebSocketDisconnect

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("mcq")

# ---------------------------------------------------------------- config
DEVICE_KEY = os.environ.get("DEVICE_KEY", "")
MODEL = os.environ.get("CLAUDE_MODEL", "claude-sonnet-5-5")
JOB_TIMEOUT_S = float(os.environ.get("JOB_TIMEOUT_S", "90"))      # whole job, press -> result
RESULT_HOLD_S = float(os.environ.get("RESULT_HOLD_S", "120"))     # uncollected result auto-closes
REQUIRED_AGREEMENT = 3
IMAGES_EXPECTED = 4
MAX_IMAGE_BYTES = 3_000_000
VALID = "ABCDEFGH"

PROMPT = """This photo shows one or more multiple-choice questions on paper or a screen.

1. Find every multiple-choice question whose text and options are visible.
2. Choose ONE question:
   - If a finger or fingertip is clearly visible, choose the question it is pointing at
     (the question in the direction the fingertip points, nearest to the tip).
   - If no finger is clearly identifiable, choose the question closest to the TOP of the image.
3. Read that question and its options, and determine the correct answer.
4. Report the answer as a letter by POSITION: first option = A, second = B, and so on,
   even if the options are labeled 1/2/3/4, a/b/c/d, or not labeled at all.
5. If the chosen question or its options are too blurry, cut off, or obscured to read
   with confidence, answer "UNCLEAR". Do not guess.

Respond with ONLY this JSON, nothing else:
{"selected_by": "finger" or "top", "question_summary": "<10 words or fewer>", "answer": "A"|"B"|"C"|"D"|"E"|"F"|"G"|"H"|"UNCLEAR"}"""

client = AsyncAnthropic()  # reads ANTHROPIC_API_KEY
app = FastAPI()
lock = asyncio.Lock()
job: dict | None = None    # the single active job
camera_ws: WebSocket | None = None  # the camera board's live connection
_tasks: set = set()        # keep references to background analysis tasks


# ---------------------------------------------------------------- helpers
def check_key(key: str | None) -> None:
    if not DEVICE_KEY or key != DEVICE_KEY:
        raise HTTPException(401, "bad device key")


def _finish_locked(outcome: str, reason: str | None = None,
                   answer: str | None = None, votes: list | None = None) -> None:
    """Mark the active job done. Caller must hold `lock`."""
    job.update(
        status="done",
        done_at=time.time(),
        result={
            "outcome": outcome,                       # "answer" | "retake" | "error"
            "answer": answer,                         # "A".."H" or None
            "index": VALID.index(answer) + 1 if answer else 0,  # 1-based buzz count
            "reason": reason,
            "votes": votes or [],
        },
    )
    job["images"].clear()
    log.info("job %s done: %s", job["id"], job["result"])


def _expire_locked() -> None:
    """Time out stuck jobs so the button never gets locked out forever."""
    global job
    if job is None:
        return
    now = time.time()
    if job["status"] == "done":
        if now - job["done_at"] > RESULT_HOLD_S:
            log.warning("job %s result never collected; closing", job["id"])
            job = None
        return
    if now - job["created"] > JOB_TIMEOUT_S:
        _finish_locked("error", "job timed out")


def _get_job_locked(job_id: str) -> dict:
    _expire_locked()
    if job is None or job["id"] != job_id:
        raise HTTPException(404, "no such active job")
    return job


async def read_one(jpeg: bytes) -> str:
    """Ask the vision model about one image. Returns a letter or 'UNCLEAR'."""
    msg = await client.messages.create(
        model=MODEL,
        max_tokens=300,
        messages=[{
            "role": "user",
            "content": [
                {"type": "image", "source": {
                    "type": "base64", "media_type": "image/jpeg",
                    "data": base64.b64encode(jpeg).decode()}},
                {"type": "text", "text": PROMPT},
            ],
        }],
    )
    text = "".join(b.text for b in msg.content if b.type == "text")
    m = re.search(r"\{.*\}", text, re.S)
    if not m:
        log.warning("no JSON in model reply: %r", text[:200])
        return "UNCLEAR"
    try:
        data = json.loads(m.group(0))
    except json.JSONDecodeError:
        log.warning("bad JSON in model reply: %r", text[:200])
        return "UNCLEAR"
    log.info("image read: %s", data)
    ans = str(data.get("answer", "UNCLEAR")).strip().upper()
    return ans if len(ans) == 1 and ans in VALID else "UNCLEAR"


async def analyze(job_id: str, images: list[bytes]) -> None:
    results = await asyncio.gather(*(read_one(b) for b in images), return_exceptions=True)
    votes = []
    for r in results:
        if isinstance(r, Exception):
            log.error("vision call failed: %r", r)
            votes.append("UNCLEAR")
        else:
            votes.append(r)

    counts = Counter(v for v in votes if v != "UNCLEAR")
    async with lock:
        if job is None or job["id"] != job_id or job["status"] != "analyzing":
            return  # timed out or replaced meanwhile
        if counts:
            best, n = counts.most_common(1)[0]
            if n >= REQUIRED_AGREEMENT:
                _finish_locked("answer", answer=best, votes=votes)
                return
        _finish_locked("retake", reason=f"agreement below {REQUIRED_AGREEMENT}/{IMAGES_EXPECTED}",
                       votes=votes)


# ---------------------------------------------------------------- routes
@app.get("/")
async def health():
    async with lock:
        _expire_locked()
        return {"ok": True, "camera_connected": camera_ws is not None,
                "active_job": None if job is None else
                {"id": job["id"], "status": job["status"]}}


@app.post("/button")
async def button(request: Request, x_device_key: str | None = Header(None)):
    """Button board: create a job and push it to the camera, unless one is already running."""
    global job
    check_key(x_device_key)
    try:
        body = await request.json()
    except Exception:
        body = {}
    async with lock:
        _expire_locked()
        if job is not None:
            raise HTTPException(409, {"busy": True, "job_id": job["id"]})
        ws = camera_ws
        if ws is None:
            raise HTTPException(503, {"camera": "offline"})
        new_id = uuid.uuid4().hex[:10]
        try:
            await ws.send_json({"cmd": "capture", "job_id": new_id,
                                "shots": 8, "keep": IMAGES_EXPECTED})
        except Exception as e:
            log.warning("could not reach camera: %r", e)
            raise HTTPException(503, {"camera": "unreachable"})
        job = {
            "id": new_id,
            "status": "claimed",          # claimed -> analyzing -> done
            "created": time.time(),
            "images": {},
            "battery_pct": body.get("battery_pct"),
        }
        log.info("job %s created and sent to camera (battery %s%%)", new_id, job["battery_pct"])
        return {"job_id": new_id}


@app.websocket("/camera/ws")
async def camera_socket(ws: WebSocket):
    """Camera board keeps this open. Jobs are pushed down it as {"cmd":"capture",...}."""
    global camera_ws
    if not DEVICE_KEY or ws.headers.get("x-device-key") != DEVICE_KEY:
        await ws.close(code=1008)
        return
    await ws.accept()
    old, camera_ws = camera_ws, ws   # a reconnecting camera replaces any stale socket
    if old is not None:
        try:
            await old.close()
        except Exception:
            pass
    log.info("camera connected")
    try:
        while True:
            msg = await ws.receive_text()   # camera sends small status messages; just log them
            log.info("camera says: %s", msg[:200])
    except WebSocketDisconnect:
        pass
    except Exception as e:
        log.warning("camera socket error: %r", e)
    finally:
        if camera_ws is ws:
            camera_ws = None
            log.info("camera disconnected")


@app.post("/camera/{job_id}/image/{rank}")
async def camera_image(job_id: str, rank: int, request: Request,
                       x_device_key: str | None = Header(None)):
    check_key(x_device_key)
    if not 0 <= rank < IMAGES_EXPECTED:
        raise HTTPException(400, "rank out of range")
    data = await request.body()
    if len(data) > MAX_IMAGE_BYTES:
        raise HTTPException(413, "image too large")
    if not data.startswith(b"\xff\xd8"):
        raise HTTPException(400, "not a JPEG")
    async with lock:
        j = _get_job_locked(job_id)
        if j["status"] != "claimed":
            raise HTTPException(409, f"job is {j['status']}")
        j["images"][rank] = data
    return {"ok": True, "received": rank, "bytes": len(data)}


@app.post("/camera/{job_id}/submit")
async def camera_submit(job_id: str, x_device_key: str | None = Header(None)):
    check_key(x_device_key)
    async with lock:
        j = _get_job_locked(job_id)
        if j["status"] != "claimed":
            raise HTTPException(409, f"job is {j['status']}")
        images = [j["images"][k] for k in sorted(j["images"])]
        if len(images) < REQUIRED_AGREEMENT:
            _finish_locked("retake", reason=f"only {len(images)} images received")
            return {"ok": True, "status": "done"}
        j["status"] = "analyzing"
    task = asyncio.create_task(analyze(job_id, images))
    _tasks.add(task)
    task.add_done_callback(_tasks.discard)
    return {"ok": True, "status": "analyzing", "images": len(images)}


@app.post("/camera/{job_id}/fail")
async def camera_fail(job_id: str, request: Request, x_device_key: str | None = Header(None)):
    """Camera board couldn't capture/upload: tell the user to retake."""
    check_key(x_device_key)
    try:
        reason = (await request.json()).get("reason", "camera failure")
    except Exception:
        reason = "camera failure"
    async with lock:
        j = _get_job_locked(job_id)
        if j["status"] != "done":
            _finish_locked("retake", reason=reason)
    return {"ok": True}


@app.get("/job/{job_id}")
async def job_status(job_id: str, x_device_key: str | None = Header(None)):
    """Button board: poll for the result. Delivering a finished result closes the job."""
    global job
    check_key(x_device_key)
    async with lock:
        j = _get_job_locked(job_id)
        if j["status"] != "done":
            return {"status": j["status"]}
        out = {"status": "done", **j["result"]}
        job = None  # closed: button can create new jobs again
        log.info("job %s result delivered; closed", job_id)
        return out
