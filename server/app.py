"""
Render server for the haptic camera system.

Flow:
  1. Button node   POST /api/jobs                      -> creates a job (status "queued")
  2. Camera node   GET  /api/jobs/claim?wait=20        -> long-polls, claims the job ("capturing")
  3. Camera node   POST /api/jobs/{id}/images?rank=N   -> uploads the 4 sharpest JPEGs
  4. Camera node   POST /api/jobs/{id}/finalize        -> server runs AI analysis ("analyzing")
  5. Button node   GET  /api/jobs/{id}?wait=20         -> long-polls until "done" / "retake" / "error"

Every request must carry the header  X-API-Key: <DEVICE_API_KEY>.
"""

from __future__ import annotations

import asyncio
import base64
import json
import logging
import os
import time
import uuid
from collections import Counter
from contextlib import asynccontextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

from fastapi import Depends, FastAPI, Header, HTTPException, Query, Request, Response
from pydantic import BaseModel

log = logging.getLogger("render-server")
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

# --------------------------------------------------------------------------- config

BASE_DIR = Path(__file__).resolve().parent
CONFIG_PATH = Path(os.environ.get("ANALYSIS_CONFIG", BASE_DIR / "analysis_config.json"))
DATA_DIR = Path(os.environ.get("DATA_DIR", BASE_DIR / "data"))
DEVICE_API_KEY = os.environ.get("DEVICE_API_KEY", "change-me")
ANALYZER = os.environ.get("ANALYZER", "claude")  # "claude" or "mock" (for testing without an API key)

NUM_CAPTURES = int(os.environ.get("NUM_CAPTURES", "8"))
NUM_UPLOAD = int(os.environ.get("NUM_UPLOAD", "4"))
QUEUE_TIMEOUT_S = float(os.environ.get("QUEUE_TIMEOUT_S", "45"))    # no camera picked it up
CAPTURE_TIMEOUT_S = float(os.environ.get("CAPTURE_TIMEOUT_S", "60"))  # camera claimed but never finished
MAX_LONG_POLL_S = 25.0

UNCLEAR = "unclear"


def load_config() -> dict:
    cfg = json.loads(CONFIG_PATH.read_text())
    labels = [l["label"] for l in cfg["labels"]]
    if UNCLEAR in labels:
        raise ValueError(f'"{UNCLEAR}" is reserved; remove it from labels')
    if len(labels) != len(set(labels)):
        raise ValueError("labels must be unique")
    cfg.setdefault("min_agreement", 2)
    cfg.setdefault("model", "claude-opus-5-5")
    cfg.setdefault("effort", "low")
    return cfg


CONFIG = load_config()

# --------------------------------------------------------------------------- job store


TERMINAL = {"done", "retake", "error"}


@dataclass
class Job:
    id: str
    device_id: str
    created: float = field(default_factory=time.time)
    status: str = "queued"  # queued -> capturing -> analyzing -> done | retake | error
    camera_id: Optional[str] = None
    claimed_at: Optional[float] = None
    images: dict[int, Path] = field(default_factory=dict)  # rank -> path
    sharpness: dict[int, float] = field(default_factory=dict)
    answer: Optional[str] = None
    code: int = 0
    votes: list[str] = field(default_factory=list)
    detail: str = ""
    battery_pct: Optional[float] = None
    finished_at: Optional[float] = None
    changed: asyncio.Event = field(default_factory=asyncio.Event)

    def touch(self) -> None:
        # Wake everyone long-polling on this job, then re-arm the event.
        self.changed.set()
        self.changed = asyncio.Event()

    def public(self) -> dict:
        return {
            "job_id": self.id,
            "status": self.status,
            "answer": self.answer,
            "code": self.code,
            "votes": self.votes,
            "detail": self.detail,
        }


jobs: dict[str, Job] = {}
queue_changed = asyncio.Event()


def finish(job: Job, status: str, *, answer: str | None = None, code: int = 0, detail: str = "") -> None:
    job.status = status
    job.answer = answer
    job.code = code
    job.detail = detail
    job.finished_at = time.time()
    log.info("job %s -> %s answer=%s code=%s votes=%s %s", job.id, status, answer, code, job.votes, detail)
    job.touch()


async def watchdog() -> None:
    """Fail jobs that stall, and forget old finished jobs."""
    while True:
        now = time.time()
        for job in list(jobs.values()):
            if job.status == "queued" and now - job.created > QUEUE_TIMEOUT_S:
                finish(job, "error", detail="no camera claimed the job")
            elif job.status == "capturing" and job.claimed_at and now - job.claimed_at > CAPTURE_TIMEOUT_S:
                finish(job, "error", detail="camera did not finish uploading")
            elif job.status in TERMINAL and job.finished_at and now - job.finished_at > 3600:
                jobs.pop(job.id, None)
        await asyncio.sleep(2)


# --------------------------------------------------------------------------- AI analysis


def build_prompt() -> tuple[str, dict]:
    label_lines = "\n".join(f'- "{l["label"]}": {l["description"]}' for l in CONFIG["labels"])
    allowed = [l["label"] for l in CONFIG["labels"]] + [UNCLEAR]
    system = (
        "You classify a single photo taken by a small wearable camera. "
        "Answer the question using exactly one of the allowed labels.\n\n"
        f"Allowed labels:\n{label_lines}\n"
        f'- "{UNCLEAR}": the photo is too blurry, dark, obstructed, or ambiguous to answer confidently.\n\n'
        f'Prefer "{UNCLEAR}" over guessing: a wrong answer is worse than asking for a retake.'
    )
    schema = {
        "type": "object",
        "properties": {
            "answer": {"type": "string", "enum": allowed},
            "reason": {"type": "string"},
        },
        "required": ["answer", "reason"],
        "additionalProperties": False,
    }
    return system, schema


async def classify_claude(client, image_path: Path) -> str:
    import anthropic

    system, schema = build_prompt()
    data = base64.standard_b64encode(image_path.read_bytes()).decode("ascii")
    try:
        response = await client.beta.messages.create(
            model=CONFIG["model"],
            max_tokens=4096,
            system=system,
            betas=["server-side-fallback-2026-07-01"],
            fallbacks="default",  # re-run on a fallback model server-side if a safety classifier declines
            output_config={
                "effort": CONFIG["effort"],
                "format": {"type": "json_schema", "schema": schema},
            },
            messages=[{
                "role": "user",
                "content": [
                    {"type": "image", "source": {"type": "base64", "media_type": "image/jpeg", "data": data}},
                    {"type": "text", "text": CONFIG["question"]},
                ],
            }],
        )
    except anthropic.RateLimitError as e:
        log.warning("rate limited classifying %s: %s", image_path.name, e)
        return UNCLEAR
    except anthropic.APIStatusError as e:
        log.error("API error %s classifying %s: %s", e.status_code, image_path.name, e.message)
        return UNCLEAR
    except anthropic.APIConnectionError as e:
        log.error("connection error classifying %s: %s", image_path.name, e)
        return UNCLEAR

    if response.stop_reason in ("refusal", "max_tokens"):
        log.warning("%s: stop_reason=%s", image_path.name, response.stop_reason)
        return UNCLEAR
    text = next((b.text for b in response.content if b.type == "text"), None)
    if text is None:
        return UNCLEAR
    try:
        result = json.loads(text)
    except json.JSONDecodeError:
        return UNCLEAR
    log.info("%s -> %s (%s)", image_path.name, result.get("answer"), result.get("reason"))
    return result.get("answer", UNCLEAR)


async def classify_mock(image_path: Path) -> str:
    """Deterministic stand-in so the whole pipeline can be tested without an API key."""
    await asyncio.sleep(0.2)
    forced = os.environ.get("MOCK_ANSWER")
    if forced:
        return forced
    labels = [l["label"] for l in CONFIG["labels"]]
    return labels[image_path.stat().st_size % len(labels)]


_client = None


def get_client():
    global _client
    if _client is None:
        import anthropic

        _client = anthropic.AsyncAnthropic()
    return _client


def tally(votes: list[str]) -> tuple[Optional[str], int]:
    """Return (winning label, its code) or (None, 0) if there is no clear agreement."""
    counted = Counter(v for v in votes if v != UNCLEAR)
    if not counted:
        return None, 0
    ranking = counted.most_common()
    top_label, top_count = ranking[0]
    if top_count < CONFIG["min_agreement"]:
        return None, 0
    if len(ranking) > 1 and ranking[1][1] == top_count:  # tie for first place
        return None, 0
    labels = [l["label"] for l in CONFIG["labels"]]
    return top_label, labels.index(top_label) + 1


async def analyze(job: Job) -> None:
    try:
        paths = [job.images[r] for r in sorted(job.images)]
        if ANALYZER == "mock":
            votes = await asyncio.gather(*(classify_mock(p) for p in paths))
        else:
            client = get_client()
            votes = await asyncio.gather(*(classify_claude(client, p) for p in paths))
        job.votes = list(votes)
        (DATA_DIR / job.id / "votes.json").write_text(json.dumps({"votes": job.votes}, indent=2))
        label, code = tally(job.votes)
        if label is None:
            finish(job, "retake", detail="no agreement between photos")
        else:
            finish(job, "done", answer=label, code=code)
    except Exception as e:  # never leave the button waiting forever
        log.exception("analysis failed for %s", job.id)
        finish(job, "error", detail=f"analysis failed: {e}")


# --------------------------------------------------------------------------- HTTP API

@asynccontextmanager
async def lifespan(_app: FastAPI):
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    if DEVICE_API_KEY == "change-me":
        log.warning("DEVICE_API_KEY is the default value; set it in the environment")
    log.info("analyzer=%s labels=%s", ANALYZER, [l["label"] for l in CONFIG["labels"]])
    task = asyncio.create_task(watchdog())
    yield
    task.cancel()


app = FastAPI(title="Haptic camera render server", lifespan=lifespan)


def require_key(x_api_key: str = Header(default="")) -> None:
    if x_api_key != DEVICE_API_KEY:
        raise HTTPException(status_code=401, detail="bad api key")


def get_job(job_id: str) -> Job:
    job = jobs.get(job_id)
    if job is None:
        raise HTTPException(status_code=404, detail="unknown job")
    return job


class NewJob(BaseModel):
    device_id: str = "button"
    battery_pct: Optional[float] = None
    battery_v: Optional[float] = None


@app.get("/api/health")
async def health():
    return {"ok": True, "jobs": len(jobs)}


@app.post("/api/jobs", dependencies=[Depends(require_key)])
async def create_job(body: NewJob):
    job = Job(id=uuid.uuid4().hex[:12], device_id=body.device_id, battery_pct=body.battery_pct)
    jobs[job.id] = job
    log.info("job %s created by %s (battery %s%%)", job.id, body.device_id, body.battery_pct)
    queue_changed.set()
    return {"job_id": job.id, "status": job.status}


@app.get("/api/jobs/claim", dependencies=[Depends(require_key)])
async def claim_job(camera_id: str = "camera", wait: float = Query(20, ge=0)):
    global queue_changed
    deadline = time.monotonic() + min(wait, MAX_LONG_POLL_S)
    while True:
        queued = sorted((j for j in jobs.values() if j.status == "queued"), key=lambda j: j.created)
        if queued:
            job = queued[0]
            job.status = "capturing"
            job.camera_id = camera_id
            job.claimed_at = time.time()
            job.touch()
            log.info("job %s claimed by %s", job.id, camera_id)
            return {"job_id": job.id, "num_captures": NUM_CAPTURES, "num_upload": NUM_UPLOAD}
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return Response(status_code=204)
        queue_changed.clear()
        try:
            await asyncio.wait_for(queue_changed.wait(), timeout=remaining)
        except asyncio.TimeoutError:
            return Response(status_code=204)


@app.post("/api/jobs/{job_id}/images", dependencies=[Depends(require_key)])
async def upload_image(job_id: str, request: Request, rank: int = Query(..., ge=0),
                       shot: int = -1, sharpness: float = 0.0):
    job = get_job(job_id)
    if job.status != "capturing":
        raise HTTPException(status_code=409, detail=f"job is {job.status}")
    if rank >= NUM_UPLOAD:
        raise HTTPException(status_code=400, detail="rank out of range")
    body = await request.body()
    if len(body) < 4 or body[:2] != b"\xff\xd8":
        raise HTTPException(status_code=400, detail="body is not a JPEG")
    folder = DATA_DIR / job.id
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / f"rank{rank}_shot{shot}.jpg"
    path.write_bytes(body)
    job.images[rank] = path
    job.sharpness[rank] = sharpness
    log.info("job %s image rank=%d shot=%d sharpness=%.1f %d bytes", job.id, rank, shot, sharpness, len(body))
    return {"ok": True, "received": len(job.images)}


@app.post("/api/jobs/{job_id}/finalize", dependencies=[Depends(require_key)])
async def finalize(job_id: str):
    job = get_job(job_id)
    if job.status != "capturing":
        raise HTTPException(status_code=409, detail=f"job is {job.status}")
    if not job.images:
        finish(job, "error", detail="camera uploaded no images")
        return job.public()
    job.status = "analyzing"
    job.touch()
    asyncio.create_task(analyze(job))
    return job.public()


class Failure(BaseModel):
    reason: str = "camera error"


@app.post("/api/jobs/{job_id}/fail", dependencies=[Depends(require_key)])
async def fail(job_id: str, body: Failure):
    job = get_job(job_id)
    if job.status not in TERMINAL:
        finish(job, "error", detail=body.reason)
    return job.public()


@app.get("/api/jobs/{job_id}", dependencies=[Depends(require_key)])
async def job_status(job_id: str, wait: float = Query(0, ge=0)):
    job = get_job(job_id)
    deadline = time.monotonic() + min(wait, MAX_LONG_POLL_S)
    while job.status not in TERMINAL:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        try:
            await asyncio.wait_for(job.changed.wait(), timeout=remaining)
        except asyncio.TimeoutError:
            break
    return job.public()
