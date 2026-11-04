/*
  Button board: XIAO ESP32-S3 + DRV2605L (haptic) + MAX17048 (fuel gauge) + button

  Wiring (both breakouts on the same I2C bus)
    XIAO D4 (GPIO5) -> SDA   (DRV2605L, MAX17048)
    XIAO D5 (GPIO6) -> SCL   (DRV2605L, MAX17048)
    XIAO 3V3        -> VIN/VCC of both
    XIAO GND        -> GND of both
    Button          -> between D1 and D0 (D0 is driven LOW as the button's ground)
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
// Wi-Fi networks in priority order: the first one in range wins. Add or remove lines freely.
struct WifiNet { const char* ssid; const char* pass; };
WifiNet WIFI_NETWORKS[] = {
  {"your-ssid",         "your-password"},   // 1st choice
  {"your-phone-hotspot", "hotspot-password"}, // 2nd choice
};
const int WIFI_COUNT = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);
const char* SERVER      = "https://your-service.onrender.com";  // no trailing slash
const char* DEVICE_KEY  = "change-me";                          // must match server

#define BUTTON_PIN D1
#define BUTTON_GND_PIN D0         // other side of the button; held LOW so it acts as ground
#define MOTOR_IS_LRA false        // true for an LRA (linear) motor, false for ERM (coin/eccentric)

const uint32_t RESULT_TIMEOUT_MS = 100000;  // a bit longer than server JOB_TIMEOUT_S
const uint32_t POLL_MS           = 500;
const int      ANSWER_REPEATS    = 2;
const float    LOW_BATT_PCT      = 15.0;
const uint32_t BATT_PRINT_MS     = 60000;  // battery status line in the Serial Monitor
const float    CHARGE_RATE_MIN   = 0.5;    // %/hour; above +this = charging, below -this = draining

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

// Every pattern is also printed, so the board works with nothing wired to it
void hapticAck()     { Serial.println("[haptic] tick (accepted/ready)");      effect(24); }
void hapticRetake()  { Serial.println("[haptic] RETAKE (2 long buzzes)");     repeatEffect(15, 2, 300); }
void hapticBusy()    { Serial.println("[haptic] BUSY (1 very long buzz)");    effect(16); }
void hapticError()   { Serial.println("[haptic] ERROR (4 rapid buzzes)");     repeatEffect(47, 4, 120); }
void hapticLowBatt() { Serial.println("[haptic] LOW BATTERY (3 pulses)");     repeatEffect(52, 3, 200); }

void hapticAnswer(int index) {                       // 1-based
  Serial.printf("[haptic] ANSWER %c (%d clicks)\n", 'A' + index - 1, index);
  delay(800);  // gap so the ack tick isn't counted as a click
  for (int r = 0; r < ANSWER_REPEATS; r++) {
    repeatEffect(1, index, 350);                     // Strong Click 100%
    if (r < ANSWER_REPEATS - 1) delay(2000);
  }
}

// ------------------------------------------------------------ network
bool tryNetwork(const WifiNet& n, uint32_t timeoutMs) {
  Serial.printf("Wi-Fi: trying \"%s\"...\n", n.ssid);
  WiFi.disconnect();
  WiFi.begin(n.ssid, n.pass);
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < timeoutMs) delay(100);
  if (WiFi.status() != WL_CONNECTED) return false;
  Serial.printf("Wi-Fi: connected to \"%s\", IP %s\n", n.ssid, WiFi.localIP().toString().c_str());
  return true;
}

// Connects to the highest-priority network that's in range.
bool ensureWifi(uint32_t timeoutMs = 10000) {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.disconnect();
  int found = WiFi.scanNetworks();
  bool visible[WIFI_COUNT] = {};
  for (int j = 0; j < found; j++)
    for (int i = 0; i < WIFI_COUNT; i++)
      if (WiFi.SSID(j) == WIFI_NETWORKS[i].ssid) visible[i] = true;
  WiFi.scanDelete();

  for (int i = 0; i < WIFI_COUNT; i++)             // in range, in priority order
    if (visible[i] && tryNetwork(WIFI_NETWORKS[i], timeoutMs)) return true;
  for (int i = 0; i < WIFI_COUNT; i++)             // hidden networks don't show in scans
    if (!visible[i] && tryNetwork(WIFI_NETWORKS[i], timeoutMs)) return true;
  Serial.println("Wi-Fi: no listed network could be joined");
  return false;
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

// ------------------------------------------------------------ battery status
// The MAX17048 reports how fast the charge % is changing. Rising = charging.
// It needs ~1 minute after power-up (or after plugging/unplugging USB) to settle.
const char* chargeState(float pct, float rate) {
  if (rate > CHARGE_RATE_MIN)  return "CHARGING";
  if (pct >= 99.0 && rate > -CHARGE_RATE_MIN) return "FULL / on USB";
  if (rate < -CHARGE_RATE_MIN) return "on battery (draining)";
  return "idle (settling)";
}

void printBattery() {
  if (!haveGauge) { Serial.println("battery: fuel gauge not found"); return; }
  float pct = gauge.cellPercent(), v = gauge.cellVoltage(), rate = gauge.chargeRate();
  Serial.printf("battery: %.1f%%, %.2f V, %+.1f %%/hr -> %s\n", pct, v, rate, chargeState(pct, rate));
}

// Prints a line when the state changes, plus a regular line every BATT_PRINT_MS.
void checkBattery() {
  static uint32_t lastCheck = 0, lastPrint = 0;
  static String lastState = "";
  if (!haveGauge || (lastCheck && millis() - lastCheck < 5000)) return;
  lastCheck = millis();
  String st = chargeState(gauge.cellPercent(), gauge.chargeRate());
  if (st != lastState || millis() - lastPrint >= BATT_PRINT_MS) {
    lastState = st;
    lastPrint = millis();
    printBattery();
  }
}

// ------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_GND_PIN, OUTPUT);
  digitalWrite(BUTTON_GND_PIN, LOW);
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
  Serial.println("Ready. Press the button, or type B + Enter here to simulate a press.");
  Serial.println("Type S for battery / charging status.");
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

  // Simulated press: type B (or b) in the Serial Monitor and hit Enter
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'B' || c == 'b') {
      Serial.println("--- simulated button press ---");
      handlePress();
      while (Serial.available()) Serial.read();  // ignore anything typed while busy
    } else if (c == 'S' || c == 's') {
      printBattery();
    }
  }
  checkBattery();
  delay(5);
}
