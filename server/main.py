"""
Render server for the button -> camera -> vision -> haptic proof of concept.

Flow
  0. Camera board  WS   /camera/ws   -> stays connected; server pushes jobs down it
  1. Button board  POST /button      -> creates the single active job (409 if one exists,
                                        503 if the camera isn't connected) and sends
                                        {"cmd":"capture","job_id":...} to the camera
  2. Camera board  sends each of its best 4 photos over the same WebSocket as a binary
                   message: [1 byte rank][10 bytes job id][JPEG]. Each photo is sent to the
                   vision model the moment it arrives, so all 4 are read at the same time.
  3. Camera board  sends {"event":"submit","job_id":...,"count":N} when it's done sending.
  4. The answer is decided as soon as 3 reads agree (no waiting for the 4th), or as soon
     as 3-way agreement has become impossible (retake).
  5. Button board  GET  /job/{id}    -> polls; when status == "done" it gets the result
                                        and the job is closed (button accepted again)

The older HTTP upload endpoints (/camera/{id}/image|submit|fail) still work.
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
WS_HEADER = 11  # binary image message: 1 byte rank + 10 byte job id

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
job: dict | None = None             # the single active job
camera_ws: WebSocket | None = None  # the camera board's live connection


# ---------------------------------------------------------------- helpers
def check_key(key: str | None) -> None:
    if not DEVICE_KEY or key != DEVICE_KEY:
        raise HTTPException(401, "bad device key")


def _votes_list(j: dict) -> list:
    return [j["votes"][k] for k in sorted(j["votes"])]


def _finish_locked(outcome: str, reason: str | None = None, answer: str | None = None) -> None:
    """Mark the active job done and stop any reads still running. Caller must hold `lock`."""
    me = asyncio.current_task()
    for t in job["tasks"]:
        if t is not me and not t.done():
            t.cancel()   # result no longer matters; saves time and API cost
    job.update(
        status="done",
        done_at=time.time(),
        result={
            "outcome": outcome,                       # "answer" | "retake" | "error"
            "answer": answer,                         # "A".."H" or None
            "index": VALID.index(answer) + 1 if answer else 0,  # 1-based buzz count
            "reason": reason,
            "votes": _votes_list(job),
        },
    )
    log.info("job %s done in %.1f s: %s", job["id"], job["done_at"] - job["created"], job["result"])


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


def _decide_locked(j: dict) -> None:
    """Finish early when the outcome can no longer change. Caller must hold `lock`."""
    if j["status"] == "done":
        return
    counts = Counter(v for v in j["votes"].values() if v != "UNCLEAR")
    best, n = counts.most_common(1)[0] if counts else (None, 0)
    if n >= REQUIRED_AGREEMENT:
        _finish_locked("answer", answer=best)
        return
    expected = j["expected"] if j["expected"] is not None else IMAGES_EXPECTED
    remaining = expected - len(j["votes"])
    if n + remaining < REQUIRED_AGREEMENT:
        _finish_locked("retake", reason=f"agreement below {REQUIRED_AGREEMENT}/{expected}")


async def read_one(jpeg: bytes) -> str:
    """Ask the vision model about one image. Returns a letter or 'UNCLEAR'."""
    t0 = time.time()
    msg = await client.messages.create(
        model=MODEL,
        max_tokens=200,
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
    log.info("image read in %.1f s: %s", time.time() - t0, data)
    ans = str(data.get("answer", "UNCLEAR")).strip().upper()
    return ans if len(ans) == 1 and ans in VALID else "UNCLEAR"


async def analyze_one(job_id: str, rank: int, jpeg: bytes) -> None:
    try:
        vote = await read_one(jpeg)
    except asyncio.CancelledError:
        return
    except Exception as e:
        log.error("vision call failed: %r", e)
        vote = "UNCLEAR"
    async with lock:
        if job is None or job["id"] != job_id or job["status"] == "done":
            return
        job["votes"][rank] = vote
        _decide_locked(job)


def _add_image_locked(j: dict, rank: int, data: bytes) -> str | None:
    """Start reading one image right away. Returns an error string or None."""
    if j["status"] == "done":
        return "job already finished"
    if not 0 <= rank < IMAGES_EXPECTED:
        return "rank out of range"
    if len(data) > MAX_IMAGE_BYTES:
        return "image too large"
    if not data.startswith(b"\xff\xd8"):
        return "not a JPEG"
    if rank in j["received"]:
        return None  # duplicate; ignore
    j["received"].add(rank)
    if j["first_image_at"] is None:
        j["first_image_at"] = time.time()
        log.info("job %s first photo after %.1f s", j["id"], j["first_image_at"] - j["created"])
    task = asyncio.create_task(analyze_one(j["id"], rank, data))
    j["tasks"].append(task)
    return None


def _submit_locked(j: dict) -> None:
    """Camera has sent everything it's going to send."""
    if j["status"] == "done":
        return
    j["expected"] = len(j["received"])
    log.info("job %s: camera sent %d photos", j["id"], j["expected"])
    if j["expected"] < REQUIRED_AGREEMENT:
        _finish_locked("retake", reason=f"only {j['expected']} images received")
        return
    _decide_locked(j)


# ---------------------------------------------------------------- routes
@app.get("/")
async def health():
    async with lock:
        _expire_locked()
        return {"ok": True, "camera_connected": camera_ws is not None, "model": MODEL,
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
            "status": "capturing",       # capturing -> done
            "created": time.time(),
            "first_image_at": None,
            "received": set(),           # ranks received
            "votes": {},                 # rank -> letter / "UNCLEAR"
            "expected": None,            # set when the camera says it's done sending
            "tasks": [],
            "battery_pct": body.get("battery_pct"),
        }
        log.info("job %s created and sent to camera (battery %s%%)", new_id, job["battery_pct"])
        return {"job_id": new_id}


async def _handle_ws_bytes(data: bytes) -> None:
    if len(data) <= WS_HEADER:
        log.warning("short binary message from camera (%d bytes)", len(data))
        return
    rank = data[0]
    job_id = data[1:WS_HEADER].decode("ascii", "replace")
    jpeg = data[WS_HEADER:]
    async with lock:
        _expire_locked()
        if job is None or job["id"] != job_id:
            log.warning("photo for unknown job %s ignored", job_id)
            return
        err = _add_image_locked(job, rank, jpeg)
        if err:
            log.warning("job %s photo %d rejected: %s", job_id, rank, err)
        else:
            log.info("job %s photo %d received (%d bytes), reading now", job_id, rank, len(jpeg))


async def _handle_ws_text(text: str) -> None:
    try:
        msg = json.loads(text)
    except json.JSONDecodeError:
        log.info("camera says: %s", text[:200])
        return
    event = msg.get("event")
    job_id = msg.get("job_id")
    if event in ("submit", "fail"):
        async with lock:
            _expire_locked()
            if job is None or job["id"] != job_id:
                return
            if event == "submit":
                _submit_locked(job)
            elif job["status"] != "done":
                _finish_locked("retake", reason=msg.get("reason", "camera failure"))
        return
    log.info("camera says: %s", text[:200])


@app.websocket("/camera/ws")
async def camera_socket(ws: WebSocket):
    """Camera board keeps this open. Jobs go down it; photos and status come back up it."""
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
            msg = await ws.receive()
            if msg["type"] == "websocket.disconnect":
                break
            if msg.get("bytes") is not None:
                await _handle_ws_bytes(msg["bytes"])
            elif msg.get("text") is not None:
                await _handle_ws_text(msg["text"])
    except WebSocketDisconnect:
        pass
    except Exception as e:
        log.warning("camera socket error: %r", e)
    finally:
        if camera_ws is ws:
            camera_ws = None
            log.info("camera disconnected")


# ---- older HTTP upload path (still supported)
@app.post("/camera/{job_id}/image/{rank}")
async def camera_image(job_id: str, rank: int, request: Request,
                       x_device_key: str | None = Header(None)):
    check_key(x_device_key)
    data = await request.body()
    async with lock:
        j = _get_job_locked(job_id)
        err = _add_image_locked(j, rank, data)
        if err:
            raise HTTPException(400, err)
    return {"ok": True, "received": rank, "bytes": len(data)}


@app.post("/camera/{job_id}/submit")
async def camera_submit(job_id: str, x_device_key: str | None = Header(None)):
    check_key(x_device_key)
    async with lock:
        j = _get_job_locked(job_id)
        _submit_locked(j)
        return {"ok": True, "status": j["status"]}


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
