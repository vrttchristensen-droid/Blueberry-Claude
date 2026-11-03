/*
  Camera board: XIAO ESP32-S3 Sense + OV3660 + microSD (FAT32)

  Board settings (Arduino IDE):
    Board: "XIAO_ESP32S3"
    PSRAM: "OPI PSRAM"   <-- required
  Libraries: ArduinoJson (v7). esp_camera / img_converters ship with the ESP32 core.

  Per job:
    1. Long-poll GET /camera/next and claim a job
    2. Take 8 photos, save all to /jobs/<id>/img_N.jpg
    3. Score each by text sharpness (variance of the Laplacian on a
       half-resolution decode) and log scores to /jobs/<id>/scores.txt
    4. Upload the 4 sharpest, then POST /submit
    5. On any failure POST /fail so the user gets the retake pattern
*/
#include "esp_camera.h"
#include "img_converters.h"
#include "esp_heap_caps.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>

// ------------------------------------------------------------ config
const char* WIFI_SSID  = "your-ssid";
const char* WIFI_PASS  = "your-password";
const char* SERVER     = "https://your-service.onrender.com";
const char* DEVICE_KEY = "change-me";

const int      SHOTS       = 8;
const int      KEEP        = 4;
const int      SHOT_GAP_MS = 150;      // small gap -> slightly different frames
const uint32_t WARMUP_MS   = 1200;     // auto-exposure settle time after powering the camera
const int      MIN_UPLOADS = 3;        // server needs 3/4 agreement anyway
const framesize_t FRAME    = FRAMESIZE_UXGA;  // 1600x1200; QXGA (2048x1536) is max for OV3660

#define SD_CS 21

// XIAO ESP32-S3 Sense camera pins
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  10
#define SIOD_GPIO_NUM  40
#define SIOC_GPIO_NUM  39
#define Y9_GPIO_NUM    48
#define Y8_GPIO_NUM    11
#define Y7_GPIO_NUM    12
#define Y6_GPIO_NUM    14
#define Y5_GPIO_NUM    16
#define Y4_GPIO_NUM    18
#define Y3_GPIO_NUM    17
#define Y2_GPIO_NUM    15
#define VSYNC_GPIO_NUM 38
#define HREF_GPIO_NUM  47
#define PCLK_GPIO_NUM  13

WiFiClientSecure tls;

// ------------------------------------------------------------ camera
bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;
  c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;
  c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size   = FRAME;
  c.jpeg_quality = 10;              // lower = better quality, bigger file
  c.fb_count     = 2;
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  c.grab_mode    = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) { Serial.printf("camera init failed 0x%x\n", err); return false; }

  sensor_t* s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    // Check a saved image: if text is upside down or mirrored, change these.
    // Orientation matters: "topmost question" is judged from the image.
    s->set_vflip(s, 1);
    s->set_hmirror(s, 0);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);       // text doesn't need color
  }
  return true;
}

// Variance of the Laplacian on the green channel of a half-res decode.
// Higher = crisper edges = more readable text. Returns -1 on failure.
float sharpness(camera_fb_t* fb) {
  const int w = fb->width / 2, h = fb->height / 2;
  uint8_t* buf = (uint8_t*)heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
  if (!buf) return -1;
  if (!jpg2rgb565(fb->buf, fb->len, buf, JPG_SCALE_2X)) { heap_caps_free(buf); return -1; }

  // RGB565 is written big-endian by jpg2rgb565; green = 6 bits, a decent luma proxy
  auto G = [&](int x, int y) -> int {
    const uint8_t* p = buf + ((size_t)y * w + x) * 2;
    uint16_t px = ((uint16_t)p[0] << 8) | p[1];
    return (px >> 5) & 0x3F;
  };

  double sum = 0, sum2 = 0;
  size_t n = 0;
  for (int y = 1; y < h - 1; y++) {
    for (int x = 1; x < w - 1; x++) {
      int lap = 4 * G(x, y) - G(x - 1, y) - G(x + 1, y) - G(x, y - 1) - G(x, y + 1);
      sum += lap;
      sum2 += (double)lap * lap;
      n++;
    }
  }
  heap_caps_free(buf);
  if (!n) return -1;
  double mean = sum / n;
  return (float)(sum2 / n - mean * mean);
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

int httpJson(const char* method, const String& path, const String& body, String& out,
             uint32_t timeoutMs = 10000) {
  HTTPClient http;
  http.setTimeout(timeoutMs);
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

bool uploadFile(const String& id, int rank, const String& path) {
  for (int attempt = 0; attempt < 3; attempt++) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    HTTPClient http;
    http.setTimeout(20000);
    if (!http.begin(tls, String(SERVER) + "/camera/" + id + "/image/" + rank)) { f.close(); continue; }
    http.addHeader("X-Device-Key", DEVICE_KEY);
    http.addHeader("Content-Type", "image/jpeg");
    int code = http.sendRequest("POST", &f, f.size());
    Serial.printf("upload rank %d (%s, %u B) -> %d\n", rank, path.c_str(), (unsigned)f.size(), code);
    http.end();
    f.close();
    if (code == 200) return true;
    if (code == 404 || code == 409) return false;  // job gone / wrong state: don't retry
    delay(300);
  }
  return false;
}

void reportFail(const String& id, const char* reason) {
  String resp;
  httpJson("POST", "/camera/" + id + "/fail", String("{\"reason\":\"") + reason + "\"}", resp);
}

// ------------------------------------------------------------ job
void runJob(const String& id) {
  Serial.printf("== job %s ==\n", id.c_str());
  String dir = "/jobs/" + id;
  SD.mkdir("/jobs");
  SD.mkdir(dir);

  // Camera is powered only while capturing (it runs very hot if left streaming)
  if (!initCamera()) { reportFail(id, "camera init failed"); return; }

  // Let auto-exposure / white balance settle before the real shots
  uint32_t warm = millis();
  while (millis() - warm < WARMUP_MS) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
  }

  float score[SHOTS];
  int saved = 0;
  for (int i = 0; i < SHOTS; i++) {
    score[i] = -1;
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { Serial.printf("shot %d: capture failed\n", i); continue; }

    String path = dir + "/img_" + i + ".jpg";
    File f = SD.open(path, FILE_WRITE);
    if (f) {
      size_t wrote = f.write(fb->buf, fb->len);
      f.close();
      if (wrote == fb->len) {
        score[i] = sharpness(fb);
        saved++;
      }
    }
    Serial.printf("shot %d: %u B, sharpness %.1f\n", i, (unsigned)fb->len, score[i]);
    esp_camera_fb_return(fb);
    delay(SHOT_GAP_MS);
  }
  esp_camera_deinit();  // camera off again; uploads don't need it

  // Rank indices by sharpness, best first
  int order[SHOTS];
  for (int i = 0; i < SHOTS; i++) order[i] = i;
  for (int i = 1; i < SHOTS; i++) {
    int k = order[i], j = i - 1;
    while (j >= 0 && score[order[j]] < score[k]) { order[j + 1] = order[j]; j--; }
    order[j + 1] = k;
  }

  File log = SD.open(dir + "/scores.txt", FILE_WRITE);
  if (log) {
    for (int r = 0; r < SHOTS; r++) log.printf("rank %d: img_%d sharpness %.1f\n", r, order[r], score[order[r]]);
    log.close();
  }

  if (saved < MIN_UPLOADS) { reportFail(id, "capture failed"); return; }

  int uploaded = 0;
  for (int r = 0; r < SHOTS && uploaded < KEEP; r++) {
    int i = order[r];
    if (score[i] < 0) break;
    if (uploadFile(id, uploaded, dir + "/img_" + i + ".jpg")) uploaded++;
  }

  if (uploaded < MIN_UPLOADS) { reportFail(id, "upload failed"); return; }

  String resp;
  int code = httpJson("POST", "/camera/" + id + "/submit", "", resp);
  Serial.printf("submit -> %d %s\n", code, resp.c_str());
}

// ------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(115200);
  if (!initCamera()) { delay(3000); ESP.restart(); }  // check the camera works...
  esp_camera_deinit();                                  // ...then keep it off until a job
  if (!SD.begin(SD_CS)) { Serial.println("SD mount failed (FAT32?)"); delay(3000); ESP.restart(); }

  tls.setInsecure();  // proof of concept: skips certificate check (see README)
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);   // modem sleep: much cooler, long-poll still works
  ensureWifi(15000);
  Serial.println("camera board ready");
}

void loop() {
  if (!ensureWifi()) { delay(1000); return; }
  String resp;
  // Server holds this request open up to ~25 s waiting for a job
  int code = httpJson("GET", "/camera/next", "", resp, 30000);
  if (code == 200) {
    JsonDocument d;
    if (!deserializeJson(d, resp)) {
      String id = d["job_id"] | "";
      if (id.length()) runJob(id);
    }
  } else if (code != 204) {
    Serial.printf("poll -> %d\n", code);
    delay(1000);
  }
}
