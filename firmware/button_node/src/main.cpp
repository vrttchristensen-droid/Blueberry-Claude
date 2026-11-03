// Button / haptic node
// Seeed XIAO ESP32-S3 + DRV2605L haptic driver + MAX17048 fuel gauge + 1034 coin (ERM) motor + button
//
// Wiring (both breakouts share the I2C bus):
//   XIAO D4 / GPIO5  -> SDA  (DRV2605L @0x5A, MAX17048 @0x36)
//   XIAO D5 / GPIO6  -> SCL
//   XIAO 3V3         -> VIN/VCC of both breakouts
//   XIAO GND         -> GND of both breakouts
//   DRV2605L OUT+ / OUT- -> 1034 motor red / blue leads
//   Button: XIAO D1 / GPIO2 <-> GND   (internal pull-up, pressed = LOW)
//   LiPo: XIAO BAT+/BAT- pads, and the MAX17048 breakout's battery JST in parallel
//
// Behaviour:
//   short press  -> create a job on the render server, buzz the answer when it comes back
//   long press   -> buzz the battery level (1-4 pulses = quarter charge)
//
// Haptic vocabulary:
//   ACK          one sharp click                 button press registered, job sent
//   ANSWER n     n long buzzes                   answer #n from analysis_config.json
//   RETAKE       2 x (3 fast buzzes)             photos did not agree - try again
//   ERROR        one 1.5 s continuous buzz       Wi-Fi / server / camera / timeout problem
//   LOW BATTERY  2 soft double-clicks            battery below 15 %

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Adafruit_DRV2605.h>
#include <Adafruit_MAX1704X.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#if __has_include("config.h")
#include "config.h"
#else
#error "Copy include/config.example.h to include/config.h and fill in your Wi-Fi and server settings"
#endif

static const int PIN_SDA = 5;      // D4
static const int PIN_SCL = 6;      // D5
static const int PIN_BUTTON = 2;   // D1 (RTC-capable, used as deep-sleep wake source)

static const uint32_t DEBOUNCE_MS = 30;
static const uint32_t LONG_PRESS_MS = 1500;
static const float LOW_BATTERY_PCT = 15.0f;

Adafruit_DRV2605 drv;
Adafruit_MAX17048 gauge;
bool hapticOk = false;
bool gaugeOk = false;
uint32_t lastActivity = 0;

// ------------------------------------------------------------------ haptics

// Real-time playback lets us hold the motor on for an exact duration.
void buzz(uint32_t ms, uint8_t strength = 0x7F) {
  if (!hapticOk) { delay(ms); return; }
  drv.setMode(DRV2605_MODE_REALTIME);
  drv.setRealtimeValue(strength);
  delay(ms);
  drv.setRealtimeValue(0x00);
}

// Plays one of the DRV2605 ROM effects (ERM library 1) and waits for it.
void effect(uint8_t id) {
  if (!hapticOk) return;
  drv.setMode(DRV2605_MODE_INTTRIG);
  drv.setWaveform(0, id);
  drv.setWaveform(1, 0);
  drv.go();
  delay(250);
}

void patternAck() { effect(1); }  // 1 = Strong Click 100%

void patternAnswer(int n) {
  delay(400);
  for (int i = 0; i < n; i++) {
    buzz(450);
    delay(350);
  }
}

void patternRetake() {
  delay(400);
  for (int group = 0; group < 2; group++) {
    for (int i = 0; i < 3; i++) { buzz(90); delay(90); }
    delay(500);
  }
}

void patternError() {
  delay(400);
  buzz(1500);
}

void patternLowBattery() {
  for (int i = 0; i < 2; i++) { effect(10); delay(300); }  // 10 = Double Click 100%
}

// ------------------------------------------------------------------ battery

float batteryPct() {
  if (!gaugeOk) return -1;
  float p = gauge.cellPercent();
  return constrain(p, 0.0f, 100.0f);
}

void patternBatteryLevel() {
  float p = batteryPct();
  if (p < 0) { patternError(); return; }
  int pulses = constrain((int)ceilf(p / 25.0f), 1, 4);
  Serial.printf("battery %.1f%% %.3fV -> %d pulses\n", p, gauge.cellVoltage(), pulses);
  delay(300);
  for (int i = 0; i < pulses; i++) { buzz(200, 0x60); delay(250); }
}

// ------------------------------------------------------------------ http(s)

// Works with both http:// (local server) and https:// (e.g. Render.com) addresses.
// setInsecure() skips certificate checking: traffic is still encrypted, but the board
// does not verify it is talking to the real server. Fine for a hobby project.
WiFiClient plainClient;
WiFiClientSecure secureClient;

bool beginHttp(HTTPClient &http, const String &url) {
  if (url.startsWith("https://")) {
    secureClient.setInsecure();
    return http.begin(secureClient, url);
  }
  return http.begin(plainClient, url);
}

// ------------------------------------------------------------------ network

bool ensureWifi(uint32_t timeoutMs = 15000) {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) delay(100);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("wifi ok %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("wifi failed");
  return false;
}

// Returns HTTP status (negative on transport error) and fills `out` with the body.
int httpRequest(const char *method, const String &path, const String &body, String &out, uint32_t timeoutMs) {
  HTTPClient http;
  http.setTimeout(timeoutMs);
  http.setConnectTimeout(5000);
  if (!beginHttp(http, String(SERVER_URL) + path)) return -1;
  http.addHeader("X-API-Key", API_KEY);
  int code;
  if (strcmp(method, "POST") == 0) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(body);
  } else {
    code = http.GET();
  }
  if (code > 0) out = http.getString();
  http.end();
  return code;
}

// ------------------------------------------------------------------ job flow

void runJob() {
  patternAck();
  if (!ensureWifi()) { patternError(); return; }

  JsonDocument req;
  req["device_id"] = DEVICE_ID;
  float pct = batteryPct();
  if (pct >= 0) {
    req["battery_pct"] = pct;
    req["battery_v"] = gauge.cellVoltage();
  }
  String body, resp;
  serializeJson(req, body);

  int code = httpRequest("POST", "/api/jobs", body, resp, 10000);
  if (code != 200) {
    Serial.printf("create job failed: %d %s\n", code, resp.c_str());
    patternError();
    return;
  }
  JsonDocument created;
  if (deserializeJson(created, resp)) { patternError(); return; }
  String jobId = created["job_id"].as<String>();
  Serial.printf("job %s created\n", jobId.c_str());

  // Long-poll until the job reaches a terminal state.
  uint32_t start = millis();
  while (millis() - start < RESULT_TIMEOUT_MS) {
    code = httpRequest("GET", "/api/jobs/" + jobId + "?wait=20", "", resp, 30000);
    if (code != 200) {
      Serial.printf("poll failed: %d\n", code);
      if (code == 404 || code == 401) break;
      delay(1000);
      ensureWifi();
      continue;
    }
    JsonDocument st;
    if (deserializeJson(st, resp)) { delay(500); continue; }
    String status = st["status"].as<String>();
    Serial.printf("job %s: %s\n", jobId.c_str(), resp.c_str());

    if (status == "done") {
      int n = st["code"] | 0;
      if (n >= 1) patternAnswer(n); else patternRetake();
      return;
    }
    if (status == "retake") { patternRetake(); return; }
    if (status == "error")  { patternError(); return; }
    // queued / capturing / analyzing -> keep waiting
  }
  Serial.println("gave up waiting for result");
  patternError();
}

// ------------------------------------------------------------------ sleep

void goToSleep() {
  Serial.println("idle -> deep sleep");
  Serial.flush();
  if (hapticOk) drv.writeRegister8(DRV2605_REG_MODE, 0x40);  // DRV2605 standby bit
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  rtc_gpio_pullup_en((gpio_num_t)PIN_BUTTON);
  rtc_gpio_pulldown_dis((gpio_num_t)PIN_BUTTON);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BUTTON, 0);  // wake when pressed (LOW)
  esp_deep_sleep_start();
}

// ------------------------------------------------------------------ main

void setup() {
  Serial.begin(115200);
  bool wokeByButton = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;
  if (wokeByButton) rtc_gpio_deinit((gpio_num_t)PIN_BUTTON);  // hand pin back to normal GPIO
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  Wire.begin(PIN_SDA, PIN_SCL);

  hapticOk = drv.begin();
  if (hapticOk) {
    drv.useERM();          // 1034 coin motor is an ERM, not an LRA
    drv.selectLibrary(1);  // ERM library A
    drv.setMode(DRV2605_MODE_INTTRIG);
  } else {
    Serial.println("DRV2605L not found - check wiring");
  }

  gaugeOk = gauge.begin();
  if (!gaugeOk) Serial.println("MAX17048 not found - check wiring / battery");

  if (wokeByButton) {
    runJob();
  } else {
    patternAck();  // boot ok
    ensureWifi();
  }

  float p = batteryPct();
  if (p >= 0 && p < LOW_BATTERY_PCT) patternLowBattery();
  lastActivity = millis();
}

void loop() {
  if (digitalRead(PIN_BUTTON) == LOW) {
    delay(DEBOUNCE_MS);
    if (digitalRead(PIN_BUTTON) == LOW) {
      uint32_t pressed = millis();
      bool longPress = false;
      while (digitalRead(PIN_BUTTON) == LOW) {
        if (!longPress && millis() - pressed >= LONG_PRESS_MS) {
          longPress = true;
          patternBatteryLevel();
        }
        delay(10);
      }
      if (!longPress) {
        runJob();
        float p = batteryPct();
        if (p >= 0 && p < LOW_BATTERY_PCT) patternLowBattery();
      }
      lastActivity = millis();
      delay(DEBOUNCE_MS);
    }
  }

  if (IDLE_SLEEP_MS > 0 && millis() - lastActivity > IDLE_SLEEP_MS && digitalRead(PIN_BUTTON) == HIGH) {
    goToSleep();
  }
  delay(10);
}
