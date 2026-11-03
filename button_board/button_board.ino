/*
  Button board: XIAO ESP32-S3 + DRV2605L (haptic) + MAX17048 (fuel gauge) + button

  Wiring (both breakouts on the same I2C bus)
    XIAO D4 (GPIO5) -> SDA   (DRV2605L, MAX17048)
    XIAO D5 (GPIO6) -> SCL   (DRV2605L, MAX17048)
    XIAO 3V3        -> VIN/VCC of both
    XIAO GND        -> GND of both
    Button          -> between D1 and GND (internal pull-up used)
    Motor           -> DRV2605L OUT+/OUT-

  Libraries: Adafruit DRV2605, Adafruit MAX1704X, ArduinoJson (v7)
  Board: "XIAO_ESP32S3"

  Haptic patterns
    Press accepted      : 1 sharp tick
    Answer              : N strong clicks (A=1 ... E=5), played twice
    Retake              : 2 long (750 ms) buzzes
    Busy (job running)  : 1 very long (1 s) buzz
    Error / timeout     : 4 rapid short buzzes
    Low battery         : 3 pulses (after a result, and on boot)
*/
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_DRV2605.h>
#include <Adafruit_MAX1704X.h>
#include <ArduinoJson.h>

// ------------------------------------------------------------ config
const char* WIFI_SSID   = "your-ssid";
const char* WIFI_PASS   = "your-password";
const char* SERVER      = "https://your-service.onrender.com";  // no trailing slash
const char* DEVICE_KEY  = "change-me";                          // must match server

#define BUTTON_PIN D1
#define MOTOR_IS_LRA false        // true for an LRA (linear) motor, false for ERM (coin/eccentric)

const uint32_t RESULT_TIMEOUT_MS = 100000;  // a bit longer than server JOB_TIMEOUT_S
const uint32_t POLL_MS           = 500;
const int      ANSWER_REPEATS    = 2;
const float    LOW_BATT_PCT      = 15.0;

// ------------------------------------------------------------ globals
Adafruit_DRV2605  drv;
Adafruit_MAX17048 gauge;
bool haveGauge = false;
bool haveHaptic = false;
WiFiClientSecure tls;

// ------------------------------------------------------------ haptics
void waitHaptic() {
  uint32_t t = millis();
  while ((drv.readRegister8(DRV2605_REG_GO) & 0x01) && millis() - t < 2000) delay(5);
}

void effect(uint8_t e) {
  if (!haveHaptic) return;
  drv.setWaveform(0, e);
  drv.setWaveform(1, 0);   // end of sequence
  drv.go();
  waitHaptic();
}

void repeatEffect(uint8_t e, int n, int gapMs) {
  for (int i = 0; i < n; i++) {
    effect(e);
    if (i < n - 1) delay(gapMs);
  }
}

void hapticAck()     { effect(24); }                 // Sharp Tick 1
void hapticRetake()  { repeatEffect(15, 2, 300); }   // 750 ms Alert
void hapticBusy()    { effect(16); }                 // 1000 ms Alert
void hapticError()   { repeatEffect(47, 4, 120); }   // Buzz 1
void hapticLowBatt() { repeatEffect(52, 3, 200); }   // Pulsing Strong 1

void hapticAnswer(int index) {                       // 1-based
  delay(800);  // gap so the ack tick isn't counted as a click
  for (int r = 0; r < ANSWER_REPEATS; r++) {
    repeatEffect(1, index, 350);                     // Strong Click 100%
    if (r < ANSWER_REPEATS - 1) delay(2000);
  }
}

// ------------------------------------------------------------ network
bool ensureWifi(uint32_t timeoutMs = 10000) {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < timeoutMs) delay(100);
  return WiFi.status() == WL_CONNECTED;
}

int httpRequest(const char* method, const String& path, const String& body, String& out) {
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(tls, String(SERVER) + path)) return -1;
  http.addHeader("X-Device-Key", DEVICE_KEY);
  int code;
  if (body.length()) {
    http.addHeader("Content-Type", "application/json");
    code = http.sendRequest(method, body);
  } else {
    code = http.sendRequest(method);
  }
  out = code > 0 ? http.getString() : "";
  http.end();
  return code;
}

// ------------------------------------------------------------ main action
void handlePress() {
  if (!ensureWifi()) { hapticError(); return; }

  float pct = haveGauge ? gauge.cellPercent() : -1;
  String body = "{\"battery_pct\":" + String(pct, 1) + "}";
  String resp;
  int code = httpRequest("POST", "/button", body, resp);
  Serial.printf("POST /button -> %d %s\n", code, resp.c_str());

  if (code == 409) { hapticBusy(); return; }
  if (code != 200) { hapticError(); return; }

  JsonDocument doc;
  if (deserializeJson(doc, resp)) { hapticError(); return; }
  String id = doc["job_id"] | "";
  if (!id.length()) { hapticError(); return; }
  hapticAck();

  uint32_t start = millis();
  while (millis() - start < RESULT_TIMEOUT_MS) {
    delay(POLL_MS);
    code = httpRequest("GET", "/job/" + id, "", resp);
    if (code == 404) { hapticError(); return; }   // job expired / vanished
    if (code != 200) continue;                    // transient network error: keep polling

    JsonDocument d;
    if (deserializeJson(d, resp)) continue;
    const char* status = d["status"] | "";
    if (strcmp(status, "done") != 0) continue;

    Serial.printf("result: %s\n", resp.c_str());
    const char* outcome = d["outcome"] | "error";
    int index = d["index"] | 0;
    if (strcmp(outcome, "answer") == 0 && index >= 1) hapticAnswer(index);
    else if (strcmp(outcome, "retake") == 0)          hapticRetake();
    else                                              hapticError();

    if (haveGauge && gauge.cellPercent() < LOW_BATT_PCT) { delay(1000); hapticLowBatt(); }
    return;
  }
  hapticError();  // local timeout
}

// ------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Wire.begin();  // XIAO default SDA=D4, SCL=D5

  haveHaptic = drv.begin();
  if (haveHaptic) {
    if (MOTOR_IS_LRA) { drv.useLRA(); drv.selectLibrary(6); }
    else              { drv.useERM(); drv.selectLibrary(1); }
    drv.setMode(DRV2605_MODE_INTTRIG);
  } else {
    Serial.println("DRV2605L not found");
  }

  haveGauge = gauge.begin();
  if (!haveGauge) Serial.println("MAX17048 not found");

  tls.setInsecure();  // proof of concept: skips certificate check (see README)
  WiFi.mode(WIFI_STA);
  ensureWifi(15000);
  Serial.printf("WiFi %s, battery %.1f%%\n",
                WiFi.status() == WL_CONNECTED ? "up" : "DOWN",
                haveGauge ? gauge.cellPercent() : -1.0f);

  if (haveGauge && gauge.cellPercent() < LOW_BATT_PCT) hapticLowBatt();
  else hapticAck();  // "ready"
}

void loop() {
  static int last = HIGH;
  static uint32_t lastChange = 0;
  int s = digitalRead(BUTTON_PIN);
  if (s != last && millis() - lastChange > 50) {   // 50 ms debounce
    lastChange = millis();
    last = s;
    if (s == LOW) handlePress();  // blocks until result: presses meanwhile are ignored
  }
  delay(5);
}
