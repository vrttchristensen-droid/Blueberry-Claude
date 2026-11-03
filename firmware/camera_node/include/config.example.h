// Copy this file to config.h (same folder) and fill in your values.
// config.h is git-ignored so your Wi-Fi password never gets committed.
#pragma once

// OPTIONAL: you can leave the Wi-Fi, server and password values as they are and set
// the board up from your phone instead (it opens a "Blueberry-...-Setup" Wi-Fi network).
#define WIFI_SSID      "your-wifi"
#define WIFI_PASSWORD  "your-password"

// Your server address, no trailing slash. Render example: "https://haptic-camera.onrender.com"
// A server on your own network works too, e.g. "http://192.168.1.50:8000"
#define SERVER_URL     "https://your-app-name.onrender.com"
#define API_KEY        "change-me"                  // must match DEVICE_API_KEY on the server
#define CAMERA_ID      "camera-1"

// Delay between the 8 burst shots. A little spacing gives hand shake a chance
// to settle on at least some frames, which is the whole point of picking the sharpest.
#define SHOT_INTERVAL_MS 120

// Keep every shot on the SD card (/jobs/<job_id>/shot<N>_<sharpness>.jpg).
#define SAVE_TO_SD 1
