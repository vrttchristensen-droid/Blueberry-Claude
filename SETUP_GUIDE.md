# Step-by-step setup guide

You can do almost all of this on your phone. The one exception is **Step 3**: putting the code onto the boards needs a USB cable plugged into a **computer running Chrome or Edge**, for about 2 minutes per board. A friend's laptop, a library computer, or a Chromebook all work. An Android phone with a USB-C cable *may* work in Chrome; an iPhone can't.

---

## Step 1: Put the server online (5 min, phone is fine)

1. Make up a **device password**: letters and numbers only, about 20 characters, e.g. `blueberry7Kq2mX9vR4tZ8wN3`. Save it in your notes.
2. Tap this link: **https://render.com/deploy?repo=https://github.com/vrttchristensen-droid/Blueberry-Claude**
3. Sign in to Render **with GitHub**. If it asks which repositories Render may see, allow **Blueberry-Claude**.
4. Render shows a service called **haptic-camera**. In the **DEVICE_API_KEY** box, type your device password.
5. Tap **Deploy Blueprint** (or **Apply**). Wait 2–5 minutes until it says **Live**.
6. Open the service. Copy its address from the top, e.g. `https://haptic-camera.onrender.com`. That's your **server address**. Save it in your notes.
7. Open the server address in your browser. If you see `"Haptic camera server is running"`, it worked.

> If the link doesn't work, or Render asks for a card for the Blueprint, use the manual setup at the bottom of this guide instead. It's the same result.

The server starts in **test mode**: the whole system works, but the answers are random. That's free, and it lets you check everything before paying for AI.

## Step 2: Wire the button board

| XIAO ESP32-S3 pin | Connects to |
|---|---|
| **D4** | SDA on the DRV2605L **and** SDA on the MAX17048 |
| **D5** | SCL on the DRV2605L **and** SCL on the MAX17048 |
| **3V3** | VIN on both boards |
| **GND** | GND on both boards, and one leg of the button |
| **D1** | The other leg of the button |
| **BAT+ / BAT−** pads (underneath) | LiPo battery. The MAX17048's battery input goes across the same battery. |

Motor: red wire → **OUT+**, blue wire → **OUT−** on the DRV2605L.

For the camera board, click the camera onto the Sense expansion board, clip the expansion board onto the XIAO, and insert the microSD card.

## Step 3: Install the code (computer with Chrome/Edge, 2 min per board)

1. On the computer, open **your server address + `/flash/`**, e.g. `https://haptic-camera.onrender.com/flash/`.
2. Plug in the **button board** with a USB-C *data* cable. Click **Install button board**, pick the board from the list, and click **Install**.
   - **If the board isn't in the list:** unplug it, hold the tiny **B** button, plug it back in, and let go. Then try again. When it's done, press the **R** button.
3. Plug in the **camera board**, click **Install camera board**, and do the same.

You only need the computer this once. Future code updates can be done the same way.

## Step 4: Connect each board to your Wi-Fi (phone, 2 min per board)

Do this with the boards powered (battery, or USB to anything).

**Button board:** right after installing (or switching on) it buzzes **3 slow pulses**. That means it's in setup mode.
1. On your phone, open **Settings → Wi-Fi** and join **`Blueberry-Button-Setup`**.
2. A setup page pops up. If it doesn't, open the browser and go to `192.168.4.1`. Tap **Configure WiFi**.
3. Pick your home Wi-Fi and type its password. It **must be 2.4 GHz**; these boards can't use 5 GHz.
4. **Server address:** your server address from Step 1, e.g. `https://haptic-camera.onrender.com`
5. **Device password:** your device password from Step 1
6. Tap **Save**. The board joins your Wi-Fi and gives **one click** when it's ready.

**Camera board:** after installing it opens **`Blueberry-Camera-Setup`**. Join it from your phone and fill in the same four things.

To redo setup later:
- **Button board:** hold the button while switching it on.
- **Camera board:** press **R**, then hold **B** for the next 3 seconds.

## Step 5: Try it

Press the button. You'll feel one click, then a few seconds later some long buzzes. In test mode the number is random. On Render, the **Logs** tab shows each step happening.

## Step 6: Turn on the real AI (when your payment works)

1. At **console.anthropic.com**, add credits under **Settings → Billing**, then go to **API Keys → Create Key** and copy the key.
2. On Render, open your service → **Environment**:
   - **Add** `ANTHROPIC_API_KEY` with your key as the value
   - **Delete** `ANALYZER`
   - Tap **Save Changes**. The server restarts in about 2 minutes.

You don't need to touch the boards again.

---

## What the buzzes mean

| You feel | Meaning |
|---|---|
| 1 sharp click | Press received / board ready |
| 3 slow pulses | Setup mode: join `Blueberry-Button-Setup` on your phone |
| 1–5 long buzzes | Answer: 1 person, 2 vehicle, 3 animal, 4 food, 5 other |
| Two groups of 3 fast buzzes | **Retake.** The photos didn't agree, or were blurry or dark |
| One long 1.5 s buzz | **Error.** See below |
| Hold the button 1.5 s | Battery level: 1–4 pulses |

## If something goes wrong

| Problem | Try this |
|---|---|
| Error buzz right after the click | Wi-Fi or server problem. Hold the button while switching on, then re-check the Wi-Fi password (2.4 GHz only), the server address and the device password. |
| Error buzz about 45 s after pressing | The camera board isn't online. Re-run its phone setup. |
| Error on the first press after a long break | Render's free plan was asleep. Wait a minute and press again. |
| Always RETAKE | The photos are dark or blurry. Check the pictures on the SD card in `/jobs/`. |
| No click at all | Check the D4/D5/3V3/GND wires to the haptic board. |

## Changing what the AI looks for

On GitHub, open `server/analysis_config.json` and tap the ✏️ pencil. Change the `question` and `labels`, then tap **Commit changes**. Render updates automatically. A label's position in the list is its buzz count.

## Manual Render setup (only if the one-tap link didn't work)

On Render: **New + → Web Service**, then pick **Blueberry-Claude** and fill in:

| Field | Value |
|---|---|
| Branch | `claude/xiao-esp32-haptic-camera-vrgzl4` |
| Root Directory | `server` |
| Language | Python 3 |
| Build Command | `pip install -r requirements.txt` |
| Start Command | `uvicorn app:app --host 0.0.0.0 --port $PORT` |
| Instance Type | Free |
| Environment Variables | `DEVICE_API_KEY` = your device password, `ANALYZER` = `mock` |
| Advanced → Health Check Path | `/api/health` |

Then tap **Deploy Web Service**.

## Using the Arduino IDE instead (optional)

The single-file sketches are in `firmware/arduino/`. You also need the library **WiFiManager by tzapu**, in addition to Adafruit DRV2605, Adafruit MAX1704X and ArduinoJson. Board: **XIAO_ESP32S3**. For the camera board, set **PSRAM: OPI PSRAM**.
