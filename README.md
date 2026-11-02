# Blueberry: haptic camera

You press a button on a wearable. A second board with a camera takes a burst of photos. A server asks Claude what's in them. The answer comes back to your wrist as a vibration pattern.

```
 ┌──────────────────────┐   1. POST /api/jobs        ┌───────────────────┐
 │ Button node          │ ─────────────────────────▶ │                   │
 │ XIAO ESP32-S3        │                            │  Render server    │
 │ DRV2605L + 1034 motor│ ◀───────────────────────── │  (FastAPI +       │
 │ MAX17048 + button    │   5. long-poll result      │   Claude vision)  │
 └──────────────────────┘                            │                   │
                                                     │  4. classify each │
 ┌──────────────────────┐   2. long-poll /claim      │     photo, vote   │
 │ Camera node          │ ◀───────────────────────── │                   │
 │ XIAO ESP32-S3 Sense  │   3. upload 4 sharpest     │                   │
 │ OV3660 + 32 GB SD    │ ─────────────────────────▶ │                   │
 └──────────────────────┘                            └───────────────────┘
```

1. **Button pressed.** The button node clicks once to confirm, then creates a job on the server.
2. **Camera claims it.** The camera node is always long-polling `/api/jobs/claim`, so it picks the job up within a fraction of a second.
3. **Burst + pick sharpest.** It takes 8 photos and scores each one for sharpness (variance of the Laplacian on a ¼-scale grayscale decode). All 8 are archived to the SD card, and the 4 sharpest are uploaded.
4. **AI analysis + vote.** The server sends each of the 4 photos to Claude separately. Each call must answer with one label from `server/analysis_config.json`, or `unclear`. The label with the most votes wins, as long as it has at least `min_agreement` votes (default 2) and isn't tied.
5. **Vibrate.** The button node long-polls the job and plays the result.

## Haptic codes

| Pattern | Meaning |
|---|---|
| 1 sharp click | Button press registered (also played once at boot) |
| **N long buzzes** | Answer #N. With the default config: 1 = person, 2 = vehicle, 3 = animal, 4 = food, 5 = other |
| **2 × three fast buzzes** (bzz-bzz-bzz … bzz-bzz-bzz) | **Retake.** The photos didn't agree, or were too blurry or dark |
| **One 1.5 s continuous buzz** | **Error.** Wi-Fi, server, camera or timeout problem |
| 2 soft double-clicks | Battery below 15 % |
| Hold button 1.5 s → 1–4 pulses | Battery level in quarters |

To change what the AI looks for, edit `server/analysis_config.json`. Change the `question`, the `labels` (their order sets the buzz count), and `min_agreement`, then restart the server. Five labels or fewer keeps the buzz counts easy to tell apart.

## Wiring: button node

All I2C parts share one bus. The DRV2605L is at 0x5A and the MAX17048 is at 0x36.

| XIAO ESP32-S3 pin | Connects to |
|---|---|
| D4 / GPIO5 (SDA) | DRV2605L SDA, MAX17048 SDA |
| D5 / GPIO6 (SCL) | DRV2605L SCL, MAX17048 SCL |
| 3V3 | DRV2605L VIN, MAX17048 VIN |
| GND | DRV2605L GND, MAX17048 GND, one side of the button |
| D1 / GPIO2 | Other side of the button (internal pull-up; pressed = LOW) |
| BAT+ / BAT− pads | LiPo cell. Also connect the MAX17048 breakout's battery input across the same cell so it can measure it |

1034 motor: red lead to DRV2605L **OUT+**, blue lead to **OUT−**. The 1034 is an ERM coin motor, and the firmware configures the driver for ERM.

> The firmware targets the **MAX17048** (Adafruit's "MAX1704x" breakout and library). If you have a MAX17043/44, the readings use a different register scale, so swap in the SparkFun MAX1704x library.

The button node goes into deep sleep after 5 minutes idle (`IDLE_SLEEP_MS`). Pressing the button wakes it and starts a job straight away. Expect a 2–3 s Wi-Fi reconnect delay before the click when it's waking up.

## Camera node

Nothing to wire. The OV3660 and the microSD slot are on the Sense expansion board. Format the 32 GB card as **FAT32**. Shots are saved to `/jobs/<job_id>/shot<N>_<sharpness>.jpg`.

The SD card's chip-select is GPIO21, which is also the XIAO's orange user LED, so the firmware leaves the LED alone.

## Setup

### 1. Render server

Use a PC, a Raspberry Pi 4/5, or any machine on the same network as the boards.

```bash
cd server
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt
export ANTHROPIC_API_KEY=sk-ant-...        # your Claude API key
export DEVICE_API_KEY=some-long-random-string
uvicorn app:app --host 0.0.0.0 --port 8000
```

To test the whole chain before you spend API credits, run with `ANALYZER=mock`. It returns fake labels; set `MOCK_ANSWER=animal` to force one specific label.

The Claude requests use `claude-opus-5-5` at `low` effort, with structured output that restricts each answer to your label list. They also send `fallbacks: "default"`, so if a safety classifier declines a photo, the API re-runs it on a fallback model instead of failing. A photo that still gets no usable answer counts as `unclear`. You can change the model and effort in `analysis_config.json`.

Other environment variables: `NUM_CAPTURES` (8), `NUM_UPLOAD` (4), `QUEUE_TIMEOUT_S` (45 s with no camera → error), `CAPTURE_TIMEOUT_S` (60 s for the camera to finish → error), `DATA_DIR` (uploaded photos and votes, kept per job).

### 2. Firmware (PlatformIO)

For each board:

```bash
cd firmware/button_node            # or firmware/camera_node
cp include/config.example.h include/config.h
# edit config.h: Wi-Fi, SERVER_URL (http://<server-ip>:8000), API_KEY = DEVICE_API_KEY
pio run -t upload && pio device monitor
```

The camera node needs OPI PSRAM, which `platformio.ini` already enables. If you use the Arduino IDE instead, pick the board **XIAO_ESP32S3** and set *Tools → PSRAM → OPI PSRAM*. The button node's libraries are *Adafruit DRV2605*, *Adafruit MAX1704X* and *ArduinoJson*.

## API reference

Every request needs the `X-API-Key` header.

| Method | Path | Who | Purpose |
|---|---|---|---|
| POST | `/api/jobs` `{device_id, battery_pct, battery_v}` | button | Create a job |
| GET | `/api/jobs/claim?wait=20&camera_id=` | camera | Long-poll; `200 {job_id, num_captures, num_upload}` or `204` |
| POST | `/api/jobs/{id}/images?rank=&shot=&sharpness=` | camera | Raw JPEG body; rank 0 = sharpest |
| POST | `/api/jobs/{id}/finalize` | camera | Start the analysis |
| POST | `/api/jobs/{id}/fail` `{reason}` | camera | Report a capture/upload failure |
| GET | `/api/jobs/{id}?wait=20` | button | Long-poll; `{status, answer, code, votes, detail}` |

`status` is one of `queued → capturing → analyzing → done | retake | error`. `code` is the 1-based label index (0 unless `done`).

## Notes and limits

- The boards talk plain HTTP, so keep the server on your LAN. To reach it over the internet, put it behind HTTPS and switch the firmware to `WiFiClientSecure` with your server's CA certificate.
- Job state is held in memory, so restarting the server drops in-flight jobs. The button then times out and buzzes ERROR.
- With one camera, jobs run one at a time in press order. Extra cameras just need unique `CAMERA_ID`s.
