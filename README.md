# Multiple-choice reader: proof of concept

Three parts: a **button board** (input + haptic output), a **camera board** (capture + sharpness
ranking), and a **Render server** (job lock, vision, voting).

```
button press ──POST /button──▶ server creates job (rejects new presses while one exists)
                                   │
server ──pushes "capture" over the camera's open WebSocket──▶ camera board
   takes 8 photos → SD card → ranks by sharpness → uploads best 4 → POST /submit
                                   │
server: 4 parallel vision reads → each returns a letter or UNCLEAR
        ≥3 agree → answer   |   otherwise → retake
                                   │
button board ──GET /job/{id} (polling)──▶ gets result → vibrates → job closed
```

## Setup

### Server (Render)
1. Create a Render Blueprint from this repo (`render.yaml` at the root points at `server/`)
   (or a Web Service with the same build/start commands).
2. Set `DEVICE_KEY` (long random string) and `ANTHROPIC_API_KEY`.
3. Keep **one worker** and **one instance**: job state is in memory.
4. The free plan sleeps when idle, which adds a long delay to the first press. Use a paid plan
   for 24/7.

Optional env vars: `CLAUDE_MODEL`, `JOB_TIMEOUT_S` (90), `RESULT_HOLD_S` (120).

Opening the service URL shows `camera_connected: true` when the camera board's WebSocket is up.

### Wi-Fi networks (both boards)
At the top of each sketch, `WIFI_NETWORKS` lists networks in priority order. On connect the board
scans and joins the highest one in range. Add or remove lines as needed.

### Live view (camera board)
In the camera board's Serial Monitor type **F**: it prints `http://<board-ip>/`. Open that in a
browser on the same Wi-Fi. Type **G** to stop. Button presses still work during live view: the stream
pauses, the 8 full-resolution photos are taken, and the stream resumes. The board runs warm while
live view is on.

### Button board
Wiring and libraries are at the top of `button_board/button_board.ino`. Set Wi-Fi, `SERVER`,
`DEVICE_KEY`, and `MOTOR_IS_LRA` to match your motor.

### Camera board
Install the **WebSockets** library by Markus Sattler. Set PSRAM to **OPI PSRAM**, format the card **FAT32** (32 GB is fine), fill in the same config.
After the first job, pull the SD card and check `/jobs/<id>/`:
- images upright and not mirrored? If not, change `set_vflip` / `set_hmirror`.
  This matters, because "topmost question" is judged from the image.
- `scores.txt` shows the sharpness ranking; the top 4 should be the readable ones.

## Haptic patterns

| Meaning | Pattern |
|---|---|
| Press accepted / ready | 1 sharp tick |
| Answer | N strong clicks (A=1, B=2, C=3, D=4, E=5), played twice |
| Retake (2/4 or worse, unclear, capture failed) | 2 long buzzes |
| Busy (job already running) | 1 very long buzz |
| Error (no Wi-Fi, server down, timeout) | 4 rapid buzzes |
| Low battery (<15%) | 3 pulses |

Answers are reported **by position**: "C" means the third option, whatever its printed label.

## Failure handling
- Camera board not connected when the button is pressed → immediate error pattern.
- Any job stuck → error after 90 s, button unlocked.
- Result never collected (button board died) → auto-closed after 120 s.
- Fewer than 3 usable images → retake.

## Things to tighten past proof of concept
- **TLS:** both boards use `setInsecure()`. Pin Render's root CA with `tls.setCACert(...)`.
- **Battery:** the button board keeps Wi-Fi up. Light sleep with button wake would extend runtime
  a lot, at the cost of ~1–2 s reconnect per press.
- **Sharpness:** whole-frame Laplacian variance. If background clutter wins over the page,
  score only a center crop.
- **SD space:** each job is ~8 × 200–400 KB. Nothing is deleted automatically.
