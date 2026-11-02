// Copy this file to config.h (same folder) and fill in your values.
// config.h is git-ignored so your Wi-Fi password never gets committed.
#pragma once

#define WIFI_SSID      "your-wifi"
#define WIFI_PASSWORD  "your-password"

#define SERVER_URL     "http://192.168.1.50:8000"   // no trailing slash
#define API_KEY        "change-me"                  // must match DEVICE_API_KEY on the server
#define CAMERA_ID      "camera-1"

// Delay between the 8 burst shots. A little spacing gives hand shake a chance
// to settle on at least some frames, which is the whole point of picking the sharpest.
#define SHOT_INTERVAL_MS 120

// Keep every shot on the SD card (/jobs/<job_id>/shot<N>_<sharpness>.jpg).
#define SAVE_TO_SD 1
