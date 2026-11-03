// Copy this file to config.h (same folder) and fill in your values.
// config.h is git-ignored so your Wi-Fi password never gets committed.
#pragma once

#define WIFI_SSID      "your-wifi"
#define WIFI_PASSWORD  "your-password"

// Your server address, no trailing slash. Render example: "https://haptic-camera.onrender.com"
// A server on your own network works too, e.g. "http://192.168.1.50:8000"
#define SERVER_URL     "https://your-app-name.onrender.com"
#define API_KEY        "change-me"     // must match DEVICE_API_KEY on the server
#define DEVICE_ID      "button-1"

// Go into deep sleep after this long with no button press (0 = never sleep).
// Pressing the button wakes the board and immediately starts a new job.
#define IDLE_SLEEP_MS  (5UL * 60UL * 1000UL)

// Give up waiting for a result after this long and buzz the ERROR pattern.
#define RESULT_TIMEOUT_MS (120UL * 1000UL)
