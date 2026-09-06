/**
 * BambooSense — agentic pipe for Bambu Lab printers
 * Board: Seeed XIAO ESP32-S3 Sense (ESP32-S3R8, 8MB PSRAM, OV2640 cam, microSD)
 *
 * v0.1.0:
 *  - Fake USB stick: TinyUSB MSC backed by the microSD (printer sees a U-disk)
 *  - Bed camera: OV2640 /snapshot + /stream (point it down through the top glass)
 *  - MQTT bridge: printer's LAN broker (8883, bblp + access code) in cloud mode
 *  - OTA firmware updates over WiFi, zero-config AP provisioning portal
 */

#include <Arduino.h>
#include <string>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <SD.h>
#include <SPI.h>
#include <Update.h>
#include "USB.h"
#include "USBMSC.h"
#include "tusb.h"      // tud_connect/tud_disconnect for soft unplug/replug
#include "esp_camera.h"

#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #define WIFI_SSID_DEFAULT ""
  #define WIFI_PASS_DEFAULT ""
  #define PRN_IP_DEFAULT ""
  #define PRN_SERIAL_DEFAULT ""
  #define PRN_CODE_DEFAULT ""
#endif

#define FW_VERSION "0.1.1"

// ---------------- pins: XIAO ESP32-S3 Sense ----------------
#define PIN_CAM_XCLK   10
#define PIN_CAM_SIOD   40
#define PIN_CAM_SIOC   39
#define PIN_CAM_Y2     15
#define PIN_CAM_Y3     17
#define PIN_CAM_Y4     18
#define PIN_CAM_Y5     16
#define PIN_CAM_Y6     14
#define PIN_CAM_Y7     12
#define PIN_CAM_Y8     11
#define PIN_CAM_Y9     48
#define PIN_CAM_VSYNC  38
#define PIN_CAM_HREF   47
#define PIN_CAM_PCLK   13
// microSD (expansion board); note: onboard LED shares GPIO21 (SD CS) — flickers with SD traffic
#define PIN_SD_CS      21
#define PIN_SD_SCK     7
#define PIN_SD_MOSI    8
#define PIN_SD_MISO    9

// ---------------- globals ----------------
static Preferences prefs;
static WebServer server(80);
static WiFiClientSecure tlsClient;
static PubSubClient mqtt(tlsClient);

static SemaphoreHandle_t sdMtx;         // serializes SD access between TinyUSB task and loop()
static bool sdOK = false;
static uint32_t sdSectors = 0;
static volatile bool rebootPending = false;
static uint32_t rebootAt = 0;
static bool camOk = false;
static char devName[40] = "bamboo-sense";

static std::string lastReport;          // cached printer MQTT report
static bool mqttEnabled = false;
static String prnIp, prnSerial, prnCode;
static uint32_t mqttMsgs = 0;

static USBMSC msc;

// ---------------- tiny log ring ----------------
#define LOG_LINES 48
#define LOG_LINE  192
static char logRing[LOG_LINES][LOG_LINE];
static int  logIdx = 0;
static void blog(const char *fmt, ...) {
  char line[LOG_LINE];
  va_list args; va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  snprintf(logRing[logIdx], LOG_LINE, "[%8lus] %s", (unsigned long)(millis() / 1000), line);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.println(logRing[logIdx]);
#endif
  logIdx = (logIdx + 1) % LOG_LINES;
}

// ---------------- config helpers ----------------
static void loadConfig() {
  prefs.begin("bsense", true);
  String ssid = prefs.getString("ssid", WIFI_SSID_DEFAULT);
  String pass = prefs.getString("pass", WIFI_PASS_DEFAULT);
  String name = prefs.getString("name", "bamboo-sense");
  prnIp     = prefs.getString("pip",  PRN_IP_DEFAULT);
  prnSerial = prefs.getString("pser", PRN_SERIAL_DEFAULT);
  prnCode   = prefs.getString("pcode",PRN_CODE_DEFAULT);
  prefs.end();
  if (ssid.length()) {
    WiFi.persistent(true);
    WiFi.begin(ssid.c_str(), pass.c_str());
  }
  strlcpy(devName, name.c_str(), sizeof(devName));
  mqttEnabled = prnIp.length() && prnSerial.length() && prnCode.length();
}

// ---------------- camera ----------------
static bool camInit() {
  camera_config_t cfg = {};
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer   = LEDC_TIMER_0;
  cfg.pin_d0 = PIN_CAM_Y2;  cfg.pin_d1 = PIN_CAM_Y3;
  cfg.pin_d2 = PIN_CAM_Y4;  cfg.pin_d3 = PIN_CAM_Y5;
  cfg.pin_d4 = PIN_CAM_Y6;  cfg.pin_d5 = PIN_CAM_Y7;
  cfg.pin_d6 = PIN_CAM_Y8;  cfg.pin_d7 = PIN_CAM_Y9;
  cfg.pin_xclk = PIN_CAM_XCLK; cfg.pin_pclk = PIN_CAM_PCLK;
  cfg.pin_vsync = PIN_CAM_VSYNC; cfg.pin_href = PIN_CAM_HREF;
  cfg.pin_sccb_sda = PIN_CAM_SIOD; cfg.pin_sccb_scl = PIN_CAM_SIOC;
  cfg.pin_pwdn = -1; cfg.pin_reset = -1;
  cfg.xclk_freq_hz = 10000000;         // conservative: fewer power/EMI spikes off the printer port
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.frame_size   = FRAMESIZE_VGA;
  cfg.jpeg_quality = 12;
  cfg.fb_count = 2;
  cfg.fb_location = CAMERA_FB_IN_PSRAM;
  cfg.grab_mode = CAMERA_GRAB_LATEST;
  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) { blog("camera init FAIL 0x%x", err); return false; }
  blog("camera ready (VGA JPEG)");
  return true;
}

// ---------------- USB MSC (SD-backed fake stick) ----------------
static int32_t mscRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  if (!sdOK) return -1;
  if (xSemaphoreTake(sdMtx, pdMS_TO_TICKS(4000)) != pdTRUE) return -1;
  int32_t done = -1;
  uint64_t byteAddr = (uint64_t)lba * 512 + offset;
  uint32_t sec = byteAddr / 512;
  uint32_t off = byteAddr % 512;
  uint8_t *dst = (uint8_t *)buffer;
  uint8_t tmp[512];
  uint32_t remaining = bufsize, copied = 0;
  if (off) {                                     // partial head sector
    if (SD.readRAW(tmp, sec++)) {
      uint32_t n = 512 - off; if (n > remaining) n = remaining;
      memcpy(dst + copied, tmp + off, n); copied += n; remaining -= n;
      done = copied;
    } else { done = -1; remaining = 0; }
  }
  while (remaining >= 512 && done >= 0) {        // aligned bulk
    if (SD.readRAW(dst + copied, sec++)) { copied += 512; remaining -= 512; done = copied; }
    else { done = -1; }
  }
  if (remaining && done >= 0) {                  // partial tail sector
    if (SD.readRAW(tmp, sec++)) { memcpy(dst + copied, tmp, remaining); copied += remaining; done = copied; }
    else done = -1;
  }
  xSemaphoreGive(sdMtx);
  return done;
}

static int32_t mscWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  if (!sdOK) return -1;
  if (xSemaphoreTake(sdMtx, pdMS_TO_TICKS(4000)) != pdTRUE) return -1;
  int32_t done = -1;
  uint64_t byteAddr = (uint64_t)lba * 512 + offset;
  uint32_t sec = byteAddr / 512;
  uint32_t off = byteAddr % 512;
  uint8_t *src = (uint8_t *)buffer;
  uint8_t tmp[512];
  uint32_t remaining = bufsize, written = 0;
  if (off) {                                     // partial head sector (read-modify-write)
    if (SD.readRAW(tmp, sec)) {
      uint32_t n = 512 - off; if (n > remaining) n = remaining;
      memcpy(tmp + off, src, n);
      if (SD.writeRAW(tmp, sec++)) { written += n; remaining -= n; done = written; }
      else { done = -1; remaining = 0; }
    } else { done = -1; remaining = 0; }
  }
  while (remaining >= 512 && done >= 0) {        // aligned bulk
    if (SD.writeRAW(src + written, sec++)) { written += 512; remaining -= 512; done = written; }
    else { done = -1; }
  }
  if (remaining && done >= 0) {                  // partial tail sector (read-modify-write)
    if (SD.readRAW(tmp, sec)) {
      memcpy(tmp, src + written, remaining);
      done = SD.writeRAW(tmp, sec) ? (int32_t)(written + remaining) : -1;
    } else done = -1;
  }
  xSemaphoreGive(sdMtx);
  return done;
}

static void usbRemount() {   // eject + replug so the printer re-reads the stick
  if (!sdOK) return;
  blog("usb: remount (eject/replug)");
  tud_disconnect();
  vTaskDelay(pdMS_TO_TICKS(900));
  tud_connect();
}

// ---------------- SD + MSC bring-up ----------------
static void sdInit() {
  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  sdOK = SD.begin(PIN_SD_CS, SPI, 25000000);
  if (!sdOK) { blog("SD: not present — MSC disabled (camera/OTA still work)"); return; }
  sdSectors = (uint32_t)(SD.totalBytes() / 512);
  blog("SD: %u sectors (%.1f GB)", sdSectors, SD.totalBytes() / 1073741824.0);
  if (!SD.exists("/bamboo-sense.txt")) {
    File f = SD.open("/bamboo-sense.txt", FILE_WRITE);
    if (f) { f.println("BambooSense was here. This stick is actually an ESP32-S3."); f.close(); }
  }
  msc.vendorID("SNAIL3D");
  msc.productID("BAMBOOSENSE");
  msc.productRevision("1.0");
  msc.onRead(mscRead);
  msc.onWrite(mscWrite);
  msc.mediaPresent(true);
  msc.begin(sdSectors, 512);
}

// ---------------- MQTT bridge ----------------
static void mqttCallback(char *topic, byte *payload, unsigned int length) {
  mqttMsgs++;
  if (strstr(topic, "/report")) {
    if (length < 24000) lastReport.assign((const char *)payload, length);
  }
}

static void mqttPoll() {
  static uint32_t lastTry = 0;
  if (!mqttEnabled) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) { mqtt.loop(); return; }
  if (millis() - lastTry < 5000) return;
  lastTry = millis();
  tlsClient.setInsecure();               // LAN device, printer uses a self-signed cert
  mqtt.setServer(prnIp.c_str(), 8883);
  mqtt.setCallback(mqttCallback);
  // P2-series reports are ~19KB — buffer MUST exceed the largest report or nothing parses
  mqtt.setBufferSize(32768);
  mqtt.setKeepAlive(30);
  String cid = String(devName) + "-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX);
  bool ok = mqtt.connect(cid.c_str(), "bblp", prnCode.c_str());
  blog("mqtt %s connect %s (state=%d)", prnIp.c_str(), ok ? "OK" : "FAIL", mqtt.state());
  if (ok) {
    String repTopic = "device/" + prnSerial + "/report";
    bool subOk = mqtt.subscribe(repTopic.c_str());
    blog("mqtt subscribed %s: %s", repTopic.c_str(), subOk ? "OK" : "FAIL");
    lastReport.clear();
  }
}

// ---------------- status json ----------------
static String jsonStatus() {
  char buf[768];
  double sdGb = sdOK ? SD.totalBytes() / 1073741824.0 : 0.0;
  snprintf(buf, sizeof(buf),
    "{\"name\":\"%s\",\"fw\":\"%s\",\"ip\":\"%s\",\"ap\":%s,"
    "\"heap\":%u,\"psram\":%u,\"uptime_s\":%lu,"
    "\"sd\":%s,\"sd_gb\":%.1f,\"usb_msc\":%s,"
    "\"mqtt\":\"%s\",\"mqtt_msgs\":%u,\"printer_cfg\":%s,\"camera\":%s}",
    devName, FW_VERSION,
    WiFi.localIP().toString().c_str(),
    (WiFi.getMode() & WIFI_MODE_AP) ? "true" : "false",
    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(), (unsigned long)(millis() / 1000),
    sdOK ? "true" : "false", sdGb,
    sdOK ? "true" : "false",
    mqttEnabled ? (mqtt.connected() ? "connected" : "connecting") : "disabled",
    mqttMsgs,
    (prnIp.length() ? "true" : "false"),
    camOk ? "true" : "false");
  return String(buf);
}

// ---------------- dashboard ----------------
static const char DASHBOARD[] PROGMEM = R"HTML(<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>BambooSense</title><style>body{font-family:system-ui;margin:1rem;max-width:640px}fieldset{margin:.8rem 0}img{max-width:100%}</style></head><body>
<h2>&#128060; BambooSense</h2>
<p><a href=/snapshot>snapshot</a> &middot; <a href=/stream>stream</a> &middot; <a href=/files>files</a> &middot; <a href=/logs>logs</a> &middot; <a href=/printer>printer json</a> &middot; <a href=/status>status json</a></p>
<img src="/snapshot" width=480><br><button onclick="location.reload()">refresh</button>
<fieldset><legend>Push file to the stick</legend>
<form method=POST action=/upload enctype=multipart/form-data>
<input type=file name=f required> <input name=filename placeholder="optional name.3mf"> <button>Upload</button></form>
<button onclick="fetch('/usb/rescan').then(r=>r.text()).then(t=>alert(t))">Eject &amp; replug (printer rescan)</button></fieldset>
<fieldset><legend>WiFi</legend><form method=POST action=/config>
SSID <input name=ssid> pass <input name=pass type=password> name <input name=name placeholder=bamboo-sense> <button>Save + reboot</button></form></fieldset>
<fieldset><legend>Printer (MQTT creds)</legend><form method=POST action=/printer>
IP <input name=ip> serial <input name=serial> access code <input name=code> <button>Save</button></form></fieldset>
<fieldset><legend>Firmware</legend>
<form method=POST action=/ota enctype=multipart/form-data><input type=file name=update required> <button>OTA flash</button></form>
<button onclick="fetch('/reboot',{method:'POST'}).then(()=>alert('rebooting'))">Reboot</button></fieldset>
<p><small>BambooSense v%FW% &mdash; the steady pipe.</small></p>
</body></html>)HTML";

static File uploadFile;

// ---------------- server routes ----------------
static void setupRoutes() {
  server.on("/", HTTP_GET, []() {
    String p = DASHBOARD;
    p.replace("%FW%", FW_VERSION);
    server.send(200, "text/html", p);
  });

  server.on("/status", HTTP_GET, []() { server.send(200, "application/json", jsonStatus()); });

  server.on("/printer", HTTP_GET, []() {
    if (!mqttEnabled) { server.send(200, "application/json", "{\"mqtt\":\"disabled\"}"); return; }
    if (!lastReport.empty()) server.send(200, "application/json", lastReport.c_str());
    else server.send(200, "application/json", "{\"mqtt\":\"connected\",\"report\":\"none yet\"}");
  });

  server.on("/snapshot", HTTP_GET, []() {
    if (!camOk) { server.send(503, "text/plain", "camera not ready"); return; }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { server.send(500, "text/plain", "fb_get failed"); return; }
    WiFiClient client = server.client();
    server.setContentLength(fb->len);
    server.send(200, "image/jpeg", "");
    client.write(fb->buf, fb->len);
    esp_camera_fb_return(fb);
  });

  server.on("/stream", HTTP_GET, []() {
    if (!camOk) { server.send(503, "text/plain", "camera not ready"); return; }
    WiFiClient client = server.client();
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "multipart/x-mixed-replace; boundary=bambooframe", "");
    while (client.connected()) {
      camera_fb_t *fb = esp_camera_fb_get();
      if (!fb) break;
      client.printf("--bambooframe\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
      client.write(fb->buf, fb->len);
      client.print("\r\n");
      esp_camera_fb_return(fb);
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  });

  server.on("/files", HTTP_GET, []() {
    String out = "[";
    if (sdOK) {
      File root = SD.open("/");
      File f;
      bool first = true;
      while ((f = root.openNextFile())) {
        if (!first) out += ",";
        first = false;
        out += "{\"name\":\"" + String(f.name()) + "\",\"size\":" + String(f.size()) + "}";
        f.close();
      }
      root.close();
    }
    out += "]";
    server.send(200, "application/json", out);
  });

  server.on("/upload", HTTP_POST, []() {
    server.send(200, "text/plain", "uploaded — use /usb/rescan to make the printer rescan the stick");
  }, []() {
    HTTPUpload &up = server.upload();
    static String fname;
    if (up.status == UPLOAD_FILE_START) {
      fname = (server.hasArg("filename") && server.arg("filename").length()) ? server.arg("filename") : up.filename;
      fname.replace("/", "_"); fname.replace("\\", "_"); fname.replace("..", "_");
      if (!fname.length()) fname = "upload.bin";
      if (sdOK) {
        tud_disconnect();                // park the MSC while we write behind its back
        uploadFile = SD.open("/" + fname, FILE_WRITE);
        blog("upload start: %s", fname.c_str());
      }
    } else if (up.status == UPLOAD_FILE_WRITE) {
      if (uploadFile) uploadFile.write(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END) {
      if (uploadFile) { blog("upload done: %s (%u bytes)", fname.c_str(), (unsigned)uploadFile.size()); uploadFile.close(); }
      if (sdOK) { vTaskDelay(pdMS_TO_TICKS(300)); tud_connect(); }
    } else if (up.status == UPLOAD_FILE_ABORTED) {
      if (uploadFile) uploadFile.close();
      if (sdOK) tud_connect();
      blog("upload aborted");
    }
  });

  server.on("/usb/rescan", HTTP_POST, []() { usbRemount(); server.send(200, "text/plain", "ejected + replugged"); });

  server.on("/config", HTTP_POST, []() {
    prefs.begin("bsense", false);
    if (server.hasArg("ssid")) prefs.putString("ssid", server.arg("ssid"));
    if (server.hasArg("pass")) prefs.putString("pass", server.arg("pass"));
    if (server.hasArg("name") && server.arg("name").length()) prefs.putString("name", server.arg("name"));
    prefs.end();
    blog("config saved via web — rebooting");
    server.send(200, "text/plain", "saved, rebooting onto new WiFi");
    rebootPending = true; rebootAt = millis() + 1200;
  });

  server.on("/printer", HTTP_POST, []() {
    if (server.hasArg("ip"))     prnIp     = server.arg("ip");
    if (server.hasArg("serial")) prnSerial = server.arg("serial");
    if (server.hasArg("code"))   prnCode   = server.arg("code");
    prefs.begin("bsense", false);
    prefs.putString("pip", prnIp);
    prefs.putString("pser", prnSerial);
    prefs.putString("pcode", prnCode);
    prefs.end();
    mqttEnabled = prnIp.length() && prnSerial.length() && prnCode.length();
    lastReport.clear();
    mqtt.disconnect();
    server.send(200, "text/plain", "printer creds saved — MQTT will (re)connect");
  });

  server.on("/ota", HTTP_POST, []() {
    bool ok = Update.end(true);
    server.send(ok ? 200 : 500, "text/plain", ok ? "OTA ok, rebooting" : Update.errorString());
    if (ok) { blog("OTA flashed — rebooting"); rebootPending = true; rebootAt = millis() + 1200; }
  }, []() {
    HTTPUpload &up = server.upload();
    if (up.status == UPLOAD_FILE_START) { blog("OTA start: %s", up.filename.c_str()); Update.begin(UPDATE_SIZE_UNKNOWN); }
    else if (up.status == UPLOAD_FILE_WRITE) { if (Update.write(up.buf, up.currentSize) != up.currentSize) blog("OTA write fail"); }
    else if (up.status == UPLOAD_FILE_END) { blog("OTA end: %u bytes", (unsigned)up.totalSize); }
    else if (up.status == UPLOAD_FILE_ABORTED) { Update.abort(); blog("OTA aborted"); }
  });

  server.on("/logs", HTTP_GET, []() {
    String out;
    for (int i = 0; i < LOG_LINES; i++) {
      int idx = (logIdx + i) % LOG_LINES;
      if (logRing[idx][0]) out += logRing[idx] + String("\n");
    }
    server.send(200, "text/plain", out);
  });

  server.on("/reboot", HTTP_POST, []() {
    server.send(200, "text/plain", "rebooting");
    rebootPending = true; rebootAt = millis() + 800;
  });

  server.on("/cmd", HTTP_POST, []() {
    // raw command passthrough: body is JSON published to device/<serial>/request
    String body = server.arg("plain");
    if (!mqttEnabled || !mqtt.connected()) { server.send(503, "text/plain", "mqtt not connected"); return; }
    if (!body.length() || body.length() > 16000) { server.send(400, "text/plain", "bad body"); return; }
    String reqTopic = "device/" + prnSerial + "/request";
    bool ok = mqtt.publish(reqTopic.c_str(), body.c_str());
    blog("cmd -> %s: %s", reqTopic.c_str(), ok ? "sent" : "FAIL");
    server.send(ok ? 200 : 500, "text/plain", ok ? "sent" : "publish failed");
  });

  server.on("/light", HTTP_GET, []() {
    String state = server.hasArg("set") ? server.arg("set") : String("");
    if (!mqttEnabled || !mqtt.connected()) { server.send(503, "text/plain", "mqtt not connected"); return; }
    if (state != "on" && state != "off") { server.send(400, "text/plain", "?set=on|off"); return; }
    String reqTopic = "device/" + prnSerial + "/request";
    String payload = "{\"system\":{\"command\":\"ledctrl\",\"led_node\":\"chamber_light\",\"led_mode\":\"" + state + "\",\"led_on_time\":500,\"led_off_time\":500,\"loop_times\":0,\"interval_time\":0}}";
    bool ok = mqtt.publish(reqTopic.c_str(), payload.c_str());
    server.send(ok ? 200 : 500, "text/plain", "chamber light " + state + (ok ? "" : " (publish failed)"));
  });

  server.onNotFound([]() { server.send(404, "text/plain", "nope"); });
}

// ---------------- setup/loop ----------------
void setup() {
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.begin(115200);
  uint32_t t0 = millis(); while (!Serial && millis() - t0 < 1500) delay(10);
#endif
  blog("BambooSense v%s booting", FW_VERSION);

  sdMtx = xSemaphoreCreateMutex();
  loadConfig();
  camOk = camInit();
  sdInit();                 // also configures MSC
  USB.begin();              // TinyUSB stack up (MSC + optional CDC)

  // WiFi
  if (WiFi.status() != WL_CONNECTED) {
    blog("wifi: connecting...");
    uint32_t t1 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t1 < 20000) delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    blog("wifi: connected %s (%s)", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    if (MDNS.begin(devName)) { MDNS.addService("http", "tcp", 80); blog("mdns: %s.local", devName); }
  } else {
    uint16_t tag = (uint32_t)(ESP.getEfuseMac() >> 32) & 0xFFFF;
    char ap[40]; snprintf(ap, sizeof(ap), "%s-%04X", devName, tag);
    WiFi.mode(WIFI_AP); WiFi.softAP(ap);
    blog("wifi: FAILED — portal AP up: %s  http://192.168.4.1/", ap);
    strlcpy(devName, ap, sizeof(devName));
  }

  setupRoutes();
  server.begin();
  blog("http: serving on port 80");
}

void loop() {
  server.handleClient();
  mqttPoll();
  if (rebootPending && millis() > rebootAt) ESP.restart();
  delay(2);
}
