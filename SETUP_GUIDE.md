# Step-by-step setup guide

This guide starts from nothing and assumes no prior experience. The server runs on **Render.com**, so you don't need a computer left on.

You need:
- A computer (Windows or Mac)
- Two **USB-C data cables**. Some cables only charge; if your computer doesn't detect a board, try another cable.
- Your two XIAO boards, DRV2605L, MAX17048, 1034 motor, button, LiPo battery, and a microSD card
- About $5 for Claude API credits

---

## Part 1: Get a Claude API key (5 min)

1. Go to **https://console.anthropic.com** and sign up.
2. Open **Settings → Billing** and add credits ($5 is plenty to start). Each button press sends 4 photos for analysis, which costs a few cents.
3. Open **API Keys → Create Key**, name it `haptic-camera`, and **copy the key** (it starts with `sk-ant-`). Paste it into a note now, because it's only shown once.

## Part 2: Make up a device password (1 min)

The boards use this password to prove to your server that they're yours. Make up a long random mix of letters and numbers, for example `blueberry7Kq2mX9vR4tZ8wN3`. Avoid spaces and symbols. Save it in the same note. Below it's called **your device password**.

## Part 3: Put the server on Render.com (10 min)

1. Go to **https://render.com** → **Get Started** → **sign up with GitHub**.
2. In the dashboard, click **New +** → **Web Service**.
3. If Render asks for access to your repositories, choose **Configure account** and give it access to **Blueberry-Claude**.
4. Pick **Blueberry-Claude** → **Connect**.
5. Fill in the form exactly like this:

   | Field | Value |
   |---|---|
   | Name | `haptic-camera` (any name works; it becomes part of your web address) |
   | Language | `Python 3` |
   | Branch | `claude/xiao-esp32-haptic-camera-vrgzl4` |
   | Root Directory | `server` |
   | Build Command | `pip install -r requirements.txt` |
   | Start Command | `uvicorn app:app --host 0.0.0.0 --port $PORT` |
   | Instance Type | **Free** to start. **Starter ($7/mo)** never sleeps (see the note below) |

6. Under **Environment Variables**, add these:

   | Key | Value |
   |---|---|
   | `ANTHROPIC_API_KEY` | your `sk-ant-...` key from Part 1 |
   | `DEVICE_API_KEY` | your device password from Part 2 |
   | `ANALYZER` | `mock`. This makes the server give fake answers for your first test, so it costs nothing. You'll delete it later. |

7. Open **Advanced** and set **Health Check Path** to `/api/health`.
8. Click **Deploy Web Service**. Wait 2–5 minutes until it says **Live**.
9. Copy your address from the top of the page, e.g. `https://haptic-camera.onrender.com`. That's your **server address**.
10. **Test it:** open the server address in your browser. You should see
    `{"ok":true,"message":"Haptic camera server is running",...}`.

> **Free plan note:** Render's free plan puts the server to sleep after 15 minutes with no visitors. The camera board checks in with the server every 20 seconds, so the server stays awake as long as the camera is powered. If the camera has been off, the first button press may buzz ERROR while the server wakes up (about a minute); just press again. The Starter plan never sleeps.

## Part 4: Install the Arduino IDE (10 min)

1. Download **Arduino IDE 2** from **https://www.arduino.cc/en/software** and install it.
2. **File → Preferences** (on Mac: **Arduino IDE → Settings**). In **Additional boards manager URLs**, paste
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json` and click **OK**.
3. **Tools → Board → Boards Manager**: search `esp32` and install **esp32 by Espressif Systems**. It's a big download.
4. Click the **Library Manager** icon (the books on the left side) and install:
   - **Adafruit DRV2605 Library**. If it asks about dependencies, choose **Install All**.
   - **Adafruit MAX1704X** (also **Install All**)
   - **ArduinoJson** by Benoit Blanchon

## Part 5: Wire the button board

| XIAO ESP32-S3 pin | Connects to |
|---|---|
| **D4** | SDA on the DRV2605L **and** SDA on the MAX17048 |
| **D5** | SCL on the DRV2605L **and** SCL on the MAX17048 |
| **3V3** | VIN on both breakouts |
| **GND** | GND on both breakouts, and one leg of the button |
| **D1** | The other leg of the button |
| **BAT+ / BAT−** pads (underneath) | LiPo battery. The MAX17048's battery connector goes across the same battery. |

Motor: red wire → **OUT+** on the DRV2605L, blue wire → **OUT−**.

## Part 6: Upload the button board

1. Open the button code on GitHub:
   https://github.com/vrttchristensen-droid/Blueberry-Claude/blob/claude/xiao-esp32-haptic-camera-vrgzl4/firmware/arduino/ButtonNode/ButtonNode.ino
   Click the **Copy raw file** button (two overlapping squares, top right of the code).
2. In the Arduino IDE: **File → New Sketch**, delete everything in it, paste.
3. In the **YOUR SETTINGS** block near the top, change:
   - `WIFI_SSID` / `WIFI_PASSWORD`: your Wi-Fi. It **must be a 2.4 GHz network**, because the boards can't use 5 GHz.
   - `SERVER_URL`: your server address from Part 3, **without** a `/` at the end
   - `API_KEY`: your device password from Part 2
4. **File → Save** and name it `ButtonNode`.
5. Plug the button board in. Pick **Tools → Board → esp32 → XIAO_ESP32S3** and **Tools → Port → the port that appeared** (COM3 or similar on Windows, `usbmodem…` on Mac).
6. Set **Tools → USB CDC On Boot → Enabled**.
7. Click **Upload** (the → arrow). It takes about a minute.
   - **If the upload fails or no port shows up:** unplug the board, **hold the tiny B (BOOT) button**, plug it back in, release, and upload again. Then press the **R (RESET)** button.
8. Open **Tools → Serial Monitor** and set it to **115200 baud**. You should see `wifi ok` and feel **one click**.

## Part 7: Upload the camera board

1. Make sure the camera module is plugged into the Sense expansion board and the expansion board is clicked onto the XIAO.
2. Insert the microSD card. It must be **FAT32**; most 32 GB cards already are.
3. Open the camera code on GitHub:
   https://github.com/vrttchristensen-droid/Blueberry-Claude/blob/claude/xiao-esp32-haptic-camera-vrgzl4/firmware/arduino/CameraNode/CameraNode.ino
   Copy it into a new sketch the same way as before.
4. Fill in the same **YOUR SETTINGS** values (Wi-Fi, `SERVER_URL`, `API_KEY`). Save it as `CameraNode`.
5. Board: **XIAO_ESP32S3**. Then set:
   - **Tools → PSRAM → OPI PSRAM**. This one is important: without it the camera won't work.
   - **Tools → USB CDC On Boot → Enabled**
6. Upload, then open the Serial Monitor at 115200. You should see `SD card ... MB` and `wifi ok`.

## Part 8: First test, then turn on the real AI

1. With both boards powered, **press the button**. You should feel one click, then a few seconds later a few long buzzes. The fake mode picks the number.
2. On Render, open your service → **Logs**. You'll see the job being created, claimed, the 4 photos arriving, and the votes.
3. If it worked, turn on the real AI. On Render, go to **Environment**, delete the `ANALYZER` variable, and click **Save Changes**. Render restarts the server, which takes about 2 minutes.
4. Press again. Now Claude looks at the photos.

### What the buzzes mean

| You feel | Meaning |
|---|---|
| 1 sharp click | Press received |
| 1 long buzz | person |
| 2 long buzzes | vehicle |
| 3 long buzzes | animal |
| 4 long buzzes | food |
| 5 long buzzes | other |
| Two groups of 3 fast buzzes | **Retake.** The photos didn't agree, or were blurry or dark |
| One long 1.5 s buzz | **Error.** See the troubleshooting table below |
| Hold the button 1.5 s | Battery level: 1–4 pulses (quarters) |

### Changing what the AI looks for

On GitHub, switch to the `claude/xiao-esp32-haptic-camera-vrgzl4` branch and open `server/analysis_config.json`. Click the ✏️ pencil, edit the `question` and `labels`, then click **Commit changes**. Render redeploys automatically. A label's position in the list is its buzz count.

## Troubleshooting

| Problem | Fix |
|---|---|
| ERROR buzz right after the click | Open the button's Serial Monitor. `wifi failed` means a wrong password or a 5 GHz network. `create job failed: 401` means `API_KEY` doesn't match `DEVICE_API_KEY` on Render. A negative number means `SERVER_URL` is wrong, or the server is asleep (wait a minute and press again). |
| ERROR buzz about 45 s after pressing | The camera board isn't connected. Check its Serial Monitor. |
| Always RETAKE | The photos are too dark or blurry. Look at them on the SD card in `/jobs/…`. |
| `camera init failed` | Reseat the camera ribbon, and make sure **PSRAM = OPI PSRAM**. |
| No click at all | `DRV2605L not found` means checking the SDA/SCL/3V3/GND wiring to the haptic board. |
| `MAX17048 not found` | The fuel gauge needs a battery connected to wake up. |
