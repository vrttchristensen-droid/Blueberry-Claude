// Camera node
// Seeed XIAO ESP32-S3 Sense + OV3660 camera + 32 GB microSD (FAT32)
//
// Loop:
//   1. Long-poll the render server for a queued job and claim it.
//   2. Take a burst of 8 photos. Score each one for sharpness (variance of the Laplacian
//      on a 1/4-scale grayscale decode) and keep the 4 sharpest in PSRAM.
//      Every shot is also archived to the SD card.
//   3. Upload the 4 sharpest (rank 0 = sharpest) and ask the server to analyze them.
//
// Hardware: the camera and SD slot are on the Sense expansion board, nothing to wire.
// Format the card as FAT32 (32 GB cards ship that way). The SD chip-select is GPIO21,
// which is also the XIAO's user LED, so the LED is left alone here.

#include <Arduino.h>
#include <algorithm>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include "esp_camera.h"
#include "img_converters.h"

// ============================ YOUR SETTINGS ============================
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
// =======================================================================

// XIAO ESP32-S3 Sense camera pins (CAMERA_MODEL_XIAO_ESP32S3)
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

static const int SD_CS_PIN = 21;

static const int MAX_CAPTURES = 16;
static const int MAX_KEEP = 8;
static const framesize_t FRAME_SIZE = FRAMESIZE_UXGA;  // 1600x1200. OV3660 also does QXGA (2048x1536).
static const int JPEG_QUALITY = 10;                     // 0-63, lower = better quality / bigger files

struct Shot {
  uint8_t *buf = nullptr;
  size_t len = 0;
  float sharpness = -1;
  int index = -1;
};

bool sdOk = false;
uint8_t *rgbBuf = nullptr;   // 1/4-scale RGB565 decode, PSRAM
uint8_t *grayBuf = nullptr;  // 1/4-scale grayscale, PSRAM
size_t scaledMaxPixels = 0;

// ------------------------------------------------------------------ camera

bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;  c.pin_d1 = Y3_GPIO_NUM;  c.pin_d2 = Y4_GPIO_NUM;  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;  c.pin_d5 = Y7_GPIO_NUM;  c.pin_d6 = Y8_GPIO_NUM;  c.pin_d7 = Y9_GPIO_NUM;
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
  c.frame_size = FRAME_SIZE;
  c.jpeg_quality = JPEG_QUALITY;
  c.fb_count = 2;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;  // always hand us the newest frame, never a stale one

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("camera init failed 0x%x\n", err);
    return false;
  }
  sensor_t *s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    // Same tweaks Espressif's CameraWebServer example applies to the OV3660.
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  } else {
    Serial.printf("warning: sensor PID 0x%x is not an OV3660\n", s->id.PID);
  }
  return true;
}

// ------------------------------------------------------------------ sharpness

// Variance of the 4-neighbour Laplacian on a 1/4-scale grayscale decode.
// Blurry frames (motion blur, missed focus) have weak edges -> low variance.
float measureSharpness(const camera_fb_t *fb) {
  int w = fb->width / 4, h = fb->height / 4;
  if ((size_t)w * h > scaledMaxPixels) return -1;
  if (!jpg2rgb565(fb->buf, fb->len, rgbBuf, JPG_SCALE_4X)) return -1;

  // jpg2rgb565 stores each pixel little-endian (low byte first), see esp32-camera to_bmp.c.
  for (int i = 0; i < w * h; i++) {
    uint16_t p = rgbBuf[2 * i] | ((uint16_t)rgbBuf[2 * i + 1] << 8);
    uint8_t r = (p >> 11) << 3, g = ((p >> 5) & 0x3F) << 2, b = (p & 0x1F) << 3;
    grayBuf[i] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
  }

  double sum = 0, sumSq = 0;
  uint32_t n = 0;
  for (int y = 1; y < h - 1; y++) {
    const uint8_t *row = grayBuf + y * w;
    for (int x = 1; x < w - 1; x++) {
      int lap = (int)row[x - w] + row[x + w] + row[x - 1] + row[x + 1] - 4 * (int)row[x];
      sum += lap;
      sumSq += (double)lap * lap;
      n++;
    }
  }
  if (n == 0) return -1;
  double mean = sum / n;
  return (float)(sumSq / n - mean * mean);
}

// ------------------------------------------------------------------ network

bool ensureWifi(uint32_t timeoutMs = 15000) {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // lower latency for the long-poll
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

int postJson(const String &path, const String &body, String *out = nullptr) {
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(String(SERVER_URL) + path)) return -1;
  http.addHeader("X-API-Key", API_KEY);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  if (out && code > 0) *out = http.getString();
  http.end();
  return code;
}

void reportFailure(const String &jobId, const String &reason) {
  Serial.printf("job %s failed: %s\n", jobId.c_str(), reason.c_str());
  JsonDocument d;
  d["reason"] = reason;
  String body;
  serializeJson(d, body);
  postJson("/api/jobs/" + jobId + "/fail", body);
}

// Long-poll for a job. Returns true and fills the job details if one was claimed.
bool claimJob(String &jobId, int &numCaptures, int &numUpload) {
  HTTPClient http;
  http.setTimeout(30000);
  if (!http.begin(String(SERVER_URL) + "/api/jobs/claim?wait=20&camera_id=" + CAMERA_ID)) return false;
  http.addHeader("X-API-Key", API_KEY);
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    JsonDocument d;
    if (!deserializeJson(d, http.getString())) {
      jobId = d["job_id"].as<String>();
      numCaptures = constrain((int)(d["num_captures"] | 8), 1, MAX_CAPTURES);
      numUpload = constrain((int)(d["num_upload"] | 4), 1, MAX_KEEP);
      ok = true;
    }
  } else if (code != 204) {
    Serial.printf("claim error %d\n", code);
    delay(2000);
  }
  http.end();
  return ok;
}

bool uploadShot(const String &jobId, int rank, const Shot &s) {
  for (int attempt = 0; attempt < 2; attempt++) {
    HTTPClient http;
    http.setTimeout(20000);
    String url = String(SERVER_URL) + "/api/jobs/" + jobId + "/images?rank=" + rank +
                 "&shot=" + s.index + "&sharpness=" + String(s.sharpness, 1);
    if (!http.begin(url)) continue;
    http.addHeader("X-API-Key", API_KEY);
    http.addHeader("Content-Type", "image/jpeg");
    int code = http.POST(s.buf, s.len);
    http.end();
    if (code == 200) return true;
    Serial.printf("upload rank %d failed: %d\n", rank, code);
    if (code == 409 || code == 404) return false;  // job already timed out / gone
    ensureWifi();
  }
  return false;
}

// ------------------------------------------------------------------ SD card

void saveToSd(const String &jobId, int index, float sharpness, const camera_fb_t *fb) {
  if (!sdOk || !SAVE_TO_SD) return;
  String dir = "/jobs/" + jobId;
  if (!SD.exists(dir)) SD.mkdir(dir);
  String path = dir + "/shot" + index + "_" + String((int)sharpness) + ".jpg";
  File f = SD.open(path, FILE_WRITE);
  if (!f) { Serial.printf("SD write failed %s\n", path.c_str()); return; }
  f.write(fb->buf, fb->len);
  f.close();
}

// ------------------------------------------------------------------ job

void freeShots(Shot *keep, int n) {
  for (int i = 0; i < n; i++) {
    if (keep[i].buf) free(keep[i].buf);
    keep[i] = Shot();
  }
}

void runJob(const String &jobId, int numCaptures, int numUpload) {
  Serial.printf("job %s: capturing %d, uploading %d\n", jobId.c_str(), numCaptures, numUpload);
  Shot keep[MAX_KEEP];

  // Drop a couple of frames so auto-exposure / white balance are current.
  for (int i = 0; i < 2; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
  }

  int captured = 0;
  for (int i = 0; i < numCaptures; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { Serial.println("capture failed"); continue; }
    float sharp = measureSharpness(fb);
    Serial.printf("shot %d: %u bytes, sharpness %.1f\n", i, fb->len, sharp);
    saveToSd(jobId, i, sharp, fb);
    captured++;

    // Keep this frame if we have a free slot or it beats the weakest kept one.
    int slot = -1;
    for (int k = 0; k < numUpload; k++) {
      if (!keep[k].buf) { slot = k; break; }
    }
    if (slot < 0) {
      int weakest = 0;
      for (int k = 1; k < numUpload; k++)
        if (keep[k].sharpness < keep[weakest].sharpness) weakest = k;
      if (sharp > keep[weakest].sharpness) slot = weakest;
    }
    if (slot >= 0) {
      uint8_t *copy = (uint8_t *)ps_malloc(fb->len);
      if (copy) {
        memcpy(copy, fb->buf, fb->len);
        if (keep[slot].buf) free(keep[slot].buf);
        keep[slot].buf = copy;
        keep[slot].len = fb->len;
        keep[slot].sharpness = sharp;
        keep[slot].index = i;
      }
    }
    esp_camera_fb_return(fb);
    delay(SHOT_INTERVAL_MS);
  }

  if (captured == 0) {
    reportFailure(jobId, "camera captured no frames");
    freeShots(keep, MAX_KEEP);
    return;
  }

  // Sort kept shots sharpest first.
  std::sort(keep, keep + numUpload, [](const Shot &a, const Shot &b) {
    if (!a.buf) return false;
    if (!b.buf) return true;
    return a.sharpness > b.sharpness;
  });

  int uploaded = 0;
  for (int r = 0; r < numUpload && keep[r].buf; r++) {
    if (uploadShot(jobId, r, keep[r])) uploaded++;
  }
  freeShots(keep, MAX_KEEP);

  if (uploaded == 0) {
    reportFailure(jobId, "image upload failed");
    return;
  }
  String resp;
  int code = postJson("/api/jobs/" + jobId + "/finalize", "{}", &resp);
  Serial.printf("finalize %d %s\n", code, resp.c_str());
}

// ------------------------------------------------------------------ main

void setup() {
  Serial.begin(115200);
  delay(500);

  if (!psramFound()) {
    Serial.println("PSRAM not found - enable OPI PSRAM in the build settings");
  }
  if (!initCamera()) {
    delay(5000);
    ESP.restart();
  }

  // Sharpness buffers sized for the largest frame the OV3660 can produce (QXGA) at 1/4 scale.
  scaledMaxPixels = (2048 / 4) * (1536 / 4);
  rgbBuf = (uint8_t *)ps_malloc(scaledMaxPixels * 2);
  grayBuf = (uint8_t *)ps_malloc(scaledMaxPixels);
  if (!rgbBuf || !grayBuf) {
    Serial.println("out of PSRAM for sharpness buffers");
    delay(5000);
    ESP.restart();
  }

  sdOk = SD.begin(SD_CS_PIN);
  if (sdOk) {
    Serial.printf("SD card %llu MB\n", SD.cardSize() / (1024ULL * 1024ULL));
    if (!SD.exists("/jobs")) SD.mkdir("/jobs");
  } else {
    Serial.println("SD card not found - continuing without archive");
  }

  ensureWifi();
}

void loop() {
  if (!ensureWifi()) {
    delay(2000);
    return;
  }
  String jobId;
  int numCaptures = 8, numUpload = 4;
  if (claimJob(jobId, numCaptures, numUpload)) {
    runJob(jobId, numCaptures, numUpload);
  }
}
