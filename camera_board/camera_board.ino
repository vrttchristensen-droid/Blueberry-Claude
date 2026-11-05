/*
  Camera board: XIAO ESP32-S3 Sense + OV3660 + microSD (FAT32)

  Board settings (Arduino IDE):
    Board: "XIAO_ESP32S3"
    PSRAM: "OPI PSRAM"   <-- required
  Libraries: ArduinoJson (v7), "WebSockets" by Markus Sattler.
             esp_camera ships with the ESP32 core.

  Per job (everything below is OFF between jobs to keep the board cool):
    1. Stay connected to the server over a WebSocket; the server pushes
       {"cmd":"capture","job_id":...} when the button is pressed
    2. Fast mode on (240 MHz CPU, full-speed Wi-Fi), camera on, short exposure settle
    3. Burst: 8 photos straight into memory, then the camera is put to sleep and turned off
    4. Rank by sharpness (JPEG size: in a burst of the same scene, the sharper frame
       keeps more fine detail and compresses less, so it's bigger) and send the best 4
       over the already-open WebSocket. The server starts reading each one as it arrives.
    5. While the server reads them, save all 8 + scores.txt to the SD card
       (card mounted only for this step), then back to low-power idle (80 MHz).
    6. On any failure tell the server so the user gets the retake pattern
*/
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include <WiFi.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include "esp_http_server.h"

// ------------------------------------------------------------ config
// Wi-Fi networks in priority order: the first one in range wins. Add or remove lines freely.
struct WifiNet { const char* ssid; const char* pass; };
WifiNet WIFI_NETWORKS[] = {
  {"your-ssid",         "your-password"},   // 1st choice
  {"your-phone-hotspot", "hotspot-password"}, // 2nd choice
};
const int WIFI_COUNT = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);
const char* SERVER     = "https://your-service.onrender.com";
const char* DEVICE_KEY = "change-me";

const int      SHOTS       = 8;
const int      KEEP        = 4;
const int      MIN_UPLOADS = 3;        // server needs 3/4 agreement anyway
const uint32_t WARMUP_MS   = 800;      // auto-exposure settle time; raise if photos come out dark
const framesize_t FRAME    = FRAMESIZE_UXGA;  // 1600x1200; QXGA (2048x1536) is max for OV3660
const framesize_t STREAM_FRAME = FRAMESIZE_VGA;  // live view: 640x480 keeps it smooth

const uint32_t CPU_FAST_MHZ = 240;     // during a job / live view
const uint32_t CPU_IDLE_MHZ = 80;      // between jobs (lowest speed Wi-Fi supports)

// Heat guard (ESP32-S3 internal sensor, approximate). Type T in the Serial Monitor to read it.
const float TEMP_WARN_C   = 70.0;      // above this: live view is switched off / refused
const float TEMP_STOP_C   = 80.0;      // above this: jobs are refused (user gets retake buzz)
const float TEMP_RESUME_C = 70.0;      // jobs allowed again once it cools below this
const uint32_t TEMP_PRINT_MS = 30000;  // how often to print the temperature

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

const size_t WS_HEADER = 11;   // binary photo message: 1 byte rank + 10 byte job id + JPEG

WebSocketsClient ws;
String pendingJob;   // set by the WebSocket handler, run from loop()

// ------------------------------------------------------------ power
// Fast = full CPU speed and Wi-Fi power saving off (much quicker uploads).
// Idle = 80 MHz CPU and Wi-Fi modem sleep: far less heat while waiting for a press.
void setFast(bool fast) {
  setCpuFrequencyMhz(fast ? CPU_FAST_MHZ : CPU_IDLE_MHZ);
  WiFi.setSleep(!fast);
}

// ------------------------------------------------------------ camera
bool initCamera(framesize_t size = FRAME) {
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
  c.frame_size   = size;
  c.jpeg_quality = (size == FRAME) ? 10 : 12;  // lower = better quality, bigger file
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

// The Sense board has no camera power-down pin, so put the sensor into its own
// software standby before releasing it. (The next init resets and wakes it.)
void cameraOff() {
  sensor_t* s = esp_camera_sensor_get();
  if (s) s->set_reg(s, 0x3008, 0xff, 0x42);   // OV3660 SYSTEM_CTROL0: software power-down
  esp_camera_deinit();
}

// ------------------------------------------------------------ live view (type F = start, G = stop)
// Serves a page on http://<board-ip>/ with an MJPEG stream from port 81.
// A job during live view pauses the stream, captures at full resolution, then resumes it.
volatile bool streaming = false;   // live view mode on
volatile bool camOn = false;       // camera running in live-view resolution
SemaphoreHandle_t camLock;         // guards camera on/off vs. the stream task
httpd_handle_t pageServer = NULL, streamServer = NULL;
const size_t STREAM_BUF = 200 * 1024;

#define PART_BOUNDARY "frameboundary8675309"
static const char* STREAM_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_SEP  = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

esp_err_t pageHandler(httpd_req_t* req) {
  String ip = WiFi.localIP().toString();
  String html =
    "<!doctype html><html><head><meta name=viewport content='width=device-width'>"
    "<title>Camera live view</title><style>body{margin:0;background:#111;color:#bbb;"
    "font-family:sans-serif;text-align:center}img{max-width:100%;height:auto}</style></head><body>"
    "<p>Live view. Type G in the Serial Monitor to stop. It freezes for a moment while a job captures.</p>"
    "<img src='http://" + ip + ":81/stream'></body></html>";
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, html.c_str(), html.length());
}

esp_err_t streamHandler(httpd_req_t* req) {
  if (!streaming) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "Live view is off. Type F in the Serial Monitor.");
  }
  uint8_t* frame = (uint8_t*)heap_caps_malloc(STREAM_BUF, MALLOC_CAP_SPIRAM);
  if (!frame) return httpd_resp_send_500(req);
  httpd_resp_set_type(req, STREAM_TYPE);

  while (streaming) {
    size_t len = 0;
    xSemaphoreTake(camLock, portMAX_DELAY);
    if (camOn) {
      camera_fb_t* fb = esp_camera_fb_get();
      if (fb) {
        if (fb->len <= STREAM_BUF) { memcpy(frame, fb->buf, fb->len); len = fb->len; }
        esp_camera_fb_return(fb);
      }
    }
    xSemaphoreGive(camLock);
    if (!len) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }  // paused for a job

    char hdr[64];
    int hlen = snprintf(hdr, sizeof hdr, STREAM_PART, (unsigned)len);
    if (httpd_resp_send_chunk(req, STREAM_SEP, strlen(STREAM_SEP)) != ESP_OK) break;
    if (httpd_resp_send_chunk(req, hdr, hlen) != ESP_OK) break;
    if (httpd_resp_send_chunk(req, (const char*)frame, len) != ESP_OK) break;  // browser closed
  }
  heap_caps_free(frame);
  return ESP_OK;
}

void startServers() {
  if (pageServer) return;
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.ctrl_port = 32768;
  httpd_uri_t page = {.uri = "/", .method = HTTP_GET, .handler = pageHandler, .user_ctx = NULL};
  if (httpd_start(&pageServer, &cfg) == ESP_OK) httpd_register_uri_handler(pageServer, &page);

  cfg.server_port = 81;
  cfg.ctrl_port = 32769;
  httpd_uri_t stream = {.uri = "/stream", .method = HTTP_GET, .handler = streamHandler, .user_ctx = NULL};
  if (httpd_start(&streamServer, &cfg) == ESP_OK) httpd_register_uri_handler(streamServer, &stream);
}

void streamCameraOn() {
  xSemaphoreTake(camLock, portMAX_DELAY);
  if (!camOn && initCamera(STREAM_FRAME)) camOn = true;
  xSemaphoreGive(camLock);
}

void streamCameraOff() {
  xSemaphoreTake(camLock, portMAX_DELAY);
  if (camOn) { cameraOff(); camOn = false; }
  xSemaphoreGive(camLock);
}

float chipTemp = 0;     // latest reading, updated by checkTemp()
bool tooHot = false;    // true from TEMP_STOP_C until it cools below TEMP_RESUME_C

void startLiveView() {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("live view needs Wi-Fi"); return; }
  if (chipTemp >= TEMP_WARN_C) {
    Serial.printf("live view refused: board is %.0f C, let it cool below %.0f C\n", chipTemp, TEMP_WARN_C);
    return;
  }
  if (!streaming) {
    setFast(true);         // full speed for video
    startServers();
    streamCameraOn();
    if (!camOn) { Serial.println("live view: camera failed to start"); setFast(false); return; }
    streaming = true;
  }
  Serial.printf("LIVE VIEW ON: open http://%s/ in a browser on the same Wi-Fi. Type G to stop.\n",
                WiFi.localIP().toString().c_str());
}

void stopLiveView() {
  if (!streaming) return;
  streaming = false;
  delay(200);              // let the stream task notice and let go of the camera
  streamCameraOff();
  setFast(false);
  Serial.println("live view off");
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

void reportFail(const String& id, const char* reason) {
  String msg = String("{\"event\":\"fail\",\"job_id\":\"") + id + "\",\"reason\":\"" + reason + "\"}";
  ws.sendTXT(msg);
}

// ------------------------------------------------------------ job
// Each shot is stored with WS_HEADER bytes of room in front, so it can be sent as-is.
uint8_t* shotBuf[SHOTS];
size_t   shotLen[SHOTS];

void freeShots() {
  for (int i = 0; i < SHOTS; i++) {
    if (shotBuf[i]) heap_caps_free(shotBuf[i]);
    shotBuf[i] = nullptr;
    shotLen[i] = 0;
  }
}

// Saves all 8 photos + scores to /jobs/<id>/. The card is only mounted for this.
void saveToSd(const String& id, const int* order) {
  uint32_t t0 = millis();
  if (!SD.begin(SD_CS)) { Serial.println("SD: mount failed, photos not saved"); return; }
  String dir = "/jobs/" + id;
  SD.mkdir("/jobs");
  SD.mkdir(dir);
  int saved = 0;
  for (int i = 0; i < SHOTS; i++) {
    if (!shotBuf[i]) continue;
    File f = SD.open(dir + "/img_" + i + ".jpg", FILE_WRITE);
    if (!f) continue;
    if (f.write(shotBuf[i] + WS_HEADER, shotLen[i]) == shotLen[i]) saved++;
    f.close();
  }
  File log = SD.open(dir + "/scores.txt", FILE_WRITE);
  if (log) {
    log.println("ranked by JPEG size (bigger = more fine detail = sharper); top 4 were sent");
    for (int r = 0; r < SHOTS; r++)
      log.printf("rank %d: img_%d %u bytes%s\n", r, order[r], (unsigned)shotLen[order[r]],
                 r < KEEP && shotLen[order[r]] ? "  <- sent" : "");
    log.close();
  }
  SD.end();
  Serial.printf("SD: saved %d photos in %lu ms\n", saved, (unsigned long)(millis() - t0));
}

void runJob(const String& id) {
  uint32_t t0 = millis();
  Serial.printf("== job %s ==\n", id.c_str());
  setFast(true);

  if (!initCamera()) { reportFail(id, "camera init failed"); setFast(streaming); return; }

  // Let auto-exposure / white balance settle before the burst
  uint32_t warm = millis();
  while (millis() - warm < WARMUP_MS) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
  }

  // Burst: 8 frames straight into memory, nothing else in between
  uint32_t tBurst = millis();
  int captured = 0;
  for (int i = 0; i < SHOTS; i++) {
    shotBuf[i] = nullptr;
    shotLen[i] = 0;
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { Serial.printf("shot %d: capture failed\n", i); continue; }
    uint8_t* b = (uint8_t*)heap_caps_malloc(WS_HEADER + fb->len, MALLOC_CAP_SPIRAM);
    if (b) {
      memcpy(b + WS_HEADER, fb->buf, fb->len);
      shotBuf[i] = b;
      shotLen[i] = fb->len;
      captured++;
    }
    esp_camera_fb_return(fb);
  }
  cameraOff();   // camera asleep and released as soon as the burst is done
  Serial.printf("burst: %d photos in %lu ms (camera on for %lu ms)\n", captured,
                (unsigned long)(millis() - tBurst), (unsigned long)(millis() - warm));

  // Rank by JPEG size, biggest (sharpest) first
  int order[SHOTS];
  for (int i = 0; i < SHOTS; i++) order[i] = i;
  for (int i = 1; i < SHOTS; i++) {
    int k = order[i], j = i - 1;
    while (j >= 0 && shotLen[order[j]] < shotLen[k]) { order[j + 1] = order[j]; j--; }
    order[j + 1] = k;
  }

  if (captured < MIN_UPLOADS) {
    reportFail(id, "capture failed");
  } else {
    // Send the best 4 over the open connection; the server starts reading each on arrival
    uint32_t tSend = millis();
    int sent = 0;
    for (int r = 0; r < SHOTS && sent < KEEP; r++) {
      int i = order[r];
      if (!shotBuf[i]) break;
      shotBuf[i][0] = (uint8_t)sent;                    // rank 0..3
      memcpy(shotBuf[i] + 1, id.c_str(), 10);           // job id (always 10 chars)
      bool ok = ws.sendBIN(shotBuf[i], WS_HEADER + shotLen[i]);
      Serial.printf("sent photo %d (img_%d, %u B): %s\n", sent, i, (unsigned)shotLen[i], ok ? "ok" : "FAILED");
      if (ok) sent++;
      ws.loop();
    }
    if (sent < MIN_UPLOADS) {
      reportFail(id, "upload failed");
    } else {
      String submit = String("{\"event\":\"submit\",\"job_id\":\"") + id + "\",\"count\":" + sent + "}";
      ws.sendTXT(submit);
    }
    Serial.printf("upload: %d photos in %lu ms (press -> sent: %lu ms)\n", sent,
                  (unsigned long)(millis() - tSend), (unsigned long)(millis() - t0));
  }

  // The server is reading the photos now; save all 8 meanwhile
  saveToSd(id, order);
  freeShots();
  setFast(streaming);   // back to low-power idle (unless live view is on)
}

// ------------------------------------------------------------ server connection
void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("connected to server, waiting for jobs");
      ws.sendTXT("{\"event\":\"hello\"}");
      break;
    case WStype_DISCONNECTED:
      Serial.println("disconnected from server, reconnecting...");
      break;
    case WStype_TEXT: {
      JsonDocument d;
      if (deserializeJson(d, payload, length)) break;
      const char* cmd = d["cmd"] | "";
      const char* id  = d["job_id"] | "";
      if (strcmp(cmd, "capture") == 0 && id[0]) pendingJob = id;  // run it from loop()
      break;
    }
    default:
      break;
  }
}

// ------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(115200);
  camLock = xSemaphoreCreateMutex();
  for (int i = 0; i < SHOTS; i++) shotBuf[i] = nullptr;

  if (!initCamera()) { delay(3000); ESP.restart(); }  // check the camera works...
  cameraOff();                                          // ...then keep it off until a job
  if (SD.begin(SD_CS)) { Serial.println("SD card OK"); SD.end(); }  // check, then unmount
  else Serial.println("WARNING: SD mount failed (FAT32?). Jobs still work; photos won't be saved.");

  WiFi.mode(WIFI_STA);
  setFast(false);        // low-power idle
  ensureWifi(15000);

  String host = String(SERVER);
  host.replace("https://", "");
  host.replace("/", "");
  ws.beginSSL(host.c_str(), 443, "/camera/ws");   // proof of concept: no certificate check
  ws.setExtraHeaders((String("X-Device-Key: ") + DEVICE_KEY).c_str());
  ws.onEvent(onWsEvent);
  ws.setReconnectInterval(3000);
  ws.enableHeartbeat(15000, 5000, 2);   // ping every 15 s; drop + reconnect if 2 pongs missed
  Serial.println("camera board ready. Type F for live view, G to stop it, T for temperature.");
}

// Reads the chip temperature every 2 s and applies the heat guard.
void checkTemp() {
  static uint32_t lastRead = 0, lastPrint = 0;
  if (lastRead && millis() - lastRead < 2000) return;
  lastRead = millis();
  chipTemp = temperatureRead();

  if (streaming && chipTemp >= TEMP_WARN_C) {
    Serial.printf("HEAT: %.0f C, turning live view off\n", chipTemp);
    stopLiveView();
  }
  if (!tooHot && chipTemp >= TEMP_STOP_C) {
    tooHot = true;
    Serial.printf("HEAT: %.0f C, refusing jobs until below %.0f C\n", chipTemp, TEMP_RESUME_C);
  } else if (tooHot && chipTemp < TEMP_RESUME_C) {
    tooHot = false;
    Serial.printf("HEAT: cooled to %.0f C, jobs allowed again\n", chipTemp);
  }
  if (millis() - lastPrint >= TEMP_PRINT_MS) {
    lastPrint = millis();
    Serial.printf("temp: %.0f C%s\n", chipTemp, tooHot ? " (too hot, jobs paused)" : "");
  }
}

void loop() {
  ensureWifi();
  ws.loop();
  checkTemp();
  if (pendingJob.length()) {
    String id = pendingJob;
    pendingJob = "";
    if (tooHot) {
      Serial.printf("job %s refused: board is %.0f C\n", id.c_str(), chipTemp);
      reportFail(id, "too hot");   // button board plays the retake pattern
      return;
    }
    bool wasStreaming = streaming;
    if (wasStreaming) streamCameraOff();   // full-res capture needs the camera to itself
    runJob(id);
    if (wasStreaming && streaming) streamCameraOn();
  }

  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'F' || c == 'f') startLiveView();
    else if (c == 'G' || c == 'g') stopLiveView();
    else if (c == 'T' || c == 't') Serial.printf("temp: %.0f C%s\n", temperatureRead(),
                                                 tooHot ? " (too hot, jobs paused)" : "");
  }
  delay(10);   // let the CPU idle between checks instead of spinning flat out
}
