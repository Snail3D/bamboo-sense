/**
 * BambooSense v0.2.0 — THE SIGNING VAULT
 * Board: Seeed XIAO ESP32-S3 Sense (ESP32-S3R8, 8MB PSRAM, OV2640 cam, microSD)
 *
 * New in 0.2:
 *  - On-chip RSA identity: mints its own keypair + self-signed cert on first boot,
 *    installs it on the printer via security.app_cert_install (verified with app_cert_list)
 *  - Signs all print.* commands (insertion-order envelope, numeric sequence_id,
 *    trailing user_id — the crack recipe, docs/CRACK.md)
 *  - SSDP auto-discovery of Bambu printers; auto-bind when exactly one is found
 *  - /cmd: agent sends plain JSON; dongle signs, publishes, waits for the ack
 *  - /discover: JSON list of printers found on the LAN
 */

#include <Arduino.h>
#include <string>
#include <vector>
#include <ArduinoJson.h>
#include <algorithm>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <SD.h>
#include <SPI.h>
#include <Update.h>
#include <time.h>
#include "USB.h"
#include "USBMSC.h"
#include "tusb.h"
#include "esp_camera.h"
#include "esp_system.h"
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/md.h>
#include <mbedtls/bignum.h>
#include <mbedtls/base64.h>

#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #define WIFI_SSID_DEFAULT ""
  #define WIFI_PASS_DEFAULT ""
  #define PRN_IP_DEFAULT ""
  #define PRN_SERIAL_DEFAULT ""
  #define PRN_CODE_DEFAULT ""
#endif
#ifndef PRN_UID_DEFAULT
  #define PRN_UID_DEFAULT ""
#endif

#define FW_VERSION "0.2.2"

// ---------------- pins (XIAO ESP32-S3 Sense) ----------------
#define PIN_CAM_XCLK 10
#define PIN_CAM_SIOD 40
#define PIN_CAM_SIOC 39
#define PIN_CAM_Y2 15
#define PIN_CAM_Y3 17
#define PIN_CAM_Y4 18
#define PIN_CAM_Y5 16
#define PIN_CAM_Y6 14
#define PIN_CAM_Y7 12
#define PIN_CAM_Y8 11
#define PIN_CAM_Y9 48
#define PIN_CAM_VSYNC 38
#define PIN_CAM_HREF 47
#define PIN_CAM_PCLK 13
#define PIN_SD_CS 21
#define PIN_SD_SCK 7
#define PIN_SD_MOSI 8
#define PIN_SD_MISO 9

// ---------------- globals ----------------
static Preferences prefs;
static WebServer server(80);
static WiFiClientSecure tlsClient;
static PubSubClient mqtt(tlsClient);
static SemaphoreHandle_t sdMtx;
static bool sdOK = false;
static uint32_t sdSectors = 0;
static volatile bool rebootPending = false;
static uint32_t rebootAt = 0;
static bool camOk = false;
static char devName[40] = "bamboo-sense";

static std::string lastReport;
static bool mqttEnabled = false;
static String prnIp, prnSerial, prnCode, prnUid;
static uint32_t mqttMsgs = 0;
static USBMSC msc;

// identity
static String idCert, idKey, idCertId;      // PEMs + derived cert_id
static bool certInstalled = false;          // printer confirmed our cert

// cmd ack tracking
static volatile uint32_t ackSeq = 0;
static volatile int ackErr = -1;            // 0 = SUCCESS
static char ackResult[24] = "";
static long ackErrCode = -1;
static uint32_t cmdSeq = 100000;             // our signed-command sequence space (persisted: anti-replay)
static volatile bool pendingCertInstall = false;  // set from callback, serviced in mqttPoll

// ---------------- log ring ----------------
#define LOG_LINES 48
#define LOG_LINE 192
static char logRing[LOG_LINES][LOG_LINE];
static int logIdx = 0;
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

// ---------------- crypto: RNG ----------------
static int rngCb(void *, unsigned char *buf, size_t len) {
  esp_fill_random(buf, len);
  return 0;
}

// JSON-escape a PEM for embedding
static String pemToJson(const String &pem) {
  String out;
  out.reserve(pem.length() + 16);
  for (size_t i = 0; i < pem.length(); i++) {
    char c = pem[i];
    if (c == '\n') out += "\\n";
    else if (c == '\r') { /* skip */ }
    else if (c == '"') out += "\\\"";
    else out += c;
  }
  return out;
}

// ---------------- identity provisioning ----------------
#if __has_include("testcerts.h")
  #include "testcerts.h"
  static void certSelfTest() {
    mbedtls_x509_crt c;
    mbedtls_x509_crt_init(&c);
    int rc = mbedtls_x509_crt_parse(&c, (const unsigned char *)TEST_CERT_MINE, strlen(TEST_CERT_MINE) + 1);
    blog("selftest: parse mine len=%u rc=%d (-0x%x)", (unsigned)strlen(TEST_CERT_MINE), rc, -rc);
    mbedtls_x509_crt_free(&c);
    mbedtls_x509_crt_init(&c);
    rc = mbedtls_x509_crt_parse(&c, (const unsigned char *)TEST_CERT_BLK, strlen(TEST_CERT_BLK) + 1);
    blog("selftest: parse blk len=%u rc=%d (-0x%x)", (unsigned)strlen(TEST_CERT_BLK), rc, -rc);
    mbedtls_x509_crt_free(&c);
  }
#else
  static void certSelfTest() {}
#endif

// PEM armor -> DER bytes (base64 body between BEGIN/END lines)
static bool pemToDer(const String &pem, std::vector<unsigned char> &der) {
  int b = pem.indexOf("-----BEGIN"), e = pem.indexOf("-----END");
  if (b < 0 || e <= b) return false;
  int ls = pem.indexOf('\n', b);
  if (ls < 0 || ls >= e) return false;
  String body = pem.substring(ls + 1, e);
  body.replace("\n", ""); body.replace("\r", ""); body.replace(" ", "");
  while (body.length() % 4) body += "=";
  der.resize(body.length() + 8);
  size_t olen = 0;
  int brc = mbedtls_base64_decode(der.data(), der.size(), &olen, (const unsigned char *)body.c_str(), body.length());
  blog("pemToDer: body=%u b=%d e=%d rc=%d olen=%u", (unsigned)body.length(), b, e, brc, (unsigned)olen);
  if (brc != 0) return false;
  der.resize(olen);
  return olen > 0;
}

// v0.2: identity (cert+key) is provisioned once via POST /identity (PEMs), stored in NVS.
// Signing happens fully on-chip; the key never leaves the device after provisioning.
static bool deriveCertId() {
  // ESP32 SDK quirk: x509_crt_parse chokes on PEM here even with PEM_PARSE_C set —
  // feed it DER ourselves (works regardless)
  std::vector<unsigned char> der;
  if (!pemToDer(idCert, der)) { blog("crypto: pem->der fail"); return false; }
  mbedtls_x509_crt crt;
  mbedtls_x509_crt_init(&crt);
  int rc = mbedtls_x509_crt_parse(&crt, der.data(), der.size());
  if (rc != 0) { blog("crypto: der parse fail -0x%x", -rc); mbedtls_x509_crt_free(&crt); return false; }
  char serBuf[128];
  size_t olen = 0;
  // mbedtls 2.28: crt.serial = decoded serial value bytes (big-endian), tag/len stripped
  blog("certid: parse OK; serial.len=%u", (unsigned)crt.serial.len);
  if (crt.serial.len < 1 || crt.serial.len > 20) {
    blog("certid: serial length out of range");
    mbedtls_x509_crt_free(&crt); return false;
  }
  for (size_t i = 0; i < crt.serial.len && olen < sizeof(serBuf) - 2; i++)
    olen += (size_t)snprintf(serBuf + olen, 3, "%02x", crt.serial.p[i]);
  serBuf[olen] = 0;
  String serHex(serBuf);
  serHex.toLowerCase();
  while (serHex.length() < 32) serHex = "0" + serHex;   // left-pad to 128-bit
  // subject DN -> take from "CN=" to end
  char dn[256]; olen = 0;
  if (mbedtls_x509_dn_gets(dn, sizeof(dn) - 1, &crt.subject) < 0) { mbedtls_x509_crt_free(&crt); return false; }
  String subj(dn);
  int cn = subj.indexOf("CN=");
  if (cn < 0) { blog("certid: no CN in subject '%s'", dn); mbedtls_x509_crt_free(&crt); return false; }
  String cnPart = cn >= 0 ? subj.substring(cn) : subj;
  idCertId = serHex + cnPart;
  mbedtls_x509_crt_free(&crt);
  return true;
}

static bool provisionIdentity(const String &certPem, const String &keyPem) {
  // validate the key parses and is RSA
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  int rc = mbedtls_pk_parse_key(&pk, (const unsigned char *)keyPem.c_str(), keyPem.length() + 1, nullptr, 0);
  if (rc != 0 || mbedtls_pk_get_type(&pk) != MBEDTLS_PK_RSA) {
    blog("crypto: bad key (-0x%x)", -rc); mbedtls_pk_free(&pk); return false;
  }
  mbedtls_pk_free(&pk);
  String oldCert = idCert, oldKey = idKey;
  idCert = certPem; idKey = keyPem;
  if (!deriveCertId()) { idCert = oldCert; idKey = oldKey; deriveCertId(); return false; }
  prefs.begin("bsense", false);
  prefs.putString("cert", idCert);
  prefs.putString("key", idKey);
  prefs.putString("certid", idCertId);
  prefs.putBool("certinst", false);
  prefs.end();
  certInstalled = false;
  blog("crypto: identity provisioned, cert_id=%s", idCertId.c_str());
  return true;
}

static void loadIdentity() {
  prefs.begin("bsense", true);
  idCert = prefs.getString("cert", "");
  idKey = prefs.getString("key", "");
  idCertId = prefs.getString("certid", "");
  certInstalled = prefs.getBool("certinst", false);
  prefs.end();
}

// ---------------- signing ----------------
// signs '{"print":<json>,"user_id":"<uid>"}' and returns the full wire envelope
static bool signPrintCommand(const String &printJson, String &wireOut) {
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  int rc = mbedtls_pk_parse_key(&pk, (const unsigned char *)idKey.c_str(), idKey.length() + 1, nullptr, 0);
  if (rc != 0) { blog("crypto: key parse fail -0x%x", -rc); mbedtls_pk_free(&pk); return false; }

  String inner = "{\"print\":" + printJson + ",\"user_id\":\"" + prnUid + "\"}";
  unsigned char hash[32];
  mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
             (const unsigned char *)inner.c_str(), inner.length(), hash);
  unsigned char sig[512];
  size_t sigLen = 0;
  rc = mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, &sigLen, rngCb, nullptr);
  mbedtls_pk_free(&pk);
  if (rc != 0) { blog("crypto: sign fail -0x%x", -rc); return false; }

  char sigB64[((512 + 2) / 3) * 4 + 8];
  size_t olen = 0;
  mbedtls_base64_encode((unsigned char *)sigB64, sizeof(sigB64), &olen, sig, sigLen);

  String header = ",\"header\":{\"sign_ver\":\"v1.0\",\"sign_alg\":\"RSA_SHA256\",\"sign_string\":\"" +
                  String(sigB64) + "\",\"cert_id\":\"" + idCertId + "\",\"payload_len\":" + String(inner.length()) + "}";
  // splice header before the final '}'
  wireOut = inner.substring(0, inner.length() - 1) + header + "}";
  return true;
}

// ---------------- SSDP discovery ----------------
struct PrinterHit { String ip; String serial; String name; };
static std::vector<PrinterHit> discovered;

static String httpGet(const String &host, const String &path) {
  WiFiClient c;
  if (!c.connect(host.c_str(), 80)) return "";
  c.print("GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n");
  String body; uint32_t t0 = millis();
  while (millis() - t0 < 2500) {
    while (c.available()) body += (char)c.read();
    if (!c.connected()) break;
    delay(10);
  }
  int p = body.indexOf("\r\n\r\n");
  return p >= 0 ? body.substring(p + 4) : body;
}

static void xmlBetween(const String &xml, const char *tag, String &out) {
  String open = String("<") + tag + ">", close = String("</") + tag + ">";
  int a = xml.indexOf(open), b = xml.indexOf(close);
  if (a >= 0 && b > a) { a += open.length(); out = xml.substring(a, b); out.trim(); }
}

static int ssdpScan() {
  discovered.clear();
  WiFiUDP udp;
  if (!udp.begin(2021)) return 0;
  const char *msearch =
    "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1990\r\nMAN: \"ssdp:discover\"\r\nMX: 3\r\nST: urn:bambulab-com:device:3dprinter:1\r\n\r\n";
  udp.beginPacket("239.255.255.250", 1990);
  udp.write((const uint8_t *)msearch, strlen(msearch));
  udp.endPacket();
  uint32_t t0 = millis();
  char pkt[1024];
  while (millis() - t0 < 4500) {
    int n = udp.parsePacket();
    if (n) {
      int len = udp.read(pkt, sizeof(pkt) - 1);
      if (len <= 0) { delay(50); continue; }
      pkt[len] = 0;
      String resp(pkt);
      int loc = resp.indexOf("Location:");
      if (loc < 0) loc = resp.indexOf("LOCATION:");
      if (loc < 0) { delay(50); continue; }
      int eol = resp.indexOf("\r\n", loc);
      String locUrl = resp.substring(loc + 9, eol);
      locUrl.trim();
      // http://<ip>:<port>/<path>
      int h0 = locUrl.indexOf("//"); int hp = locUrl.indexOf(':', h0); int h1 = locUrl.indexOf('/', h0 + 2);
      if (h0 < 0 || h1 < 0) { delay(50); continue; }
      String ip = locUrl.substring(h0 + 2, hp > 0 && hp < h1 ? hp : h1);
      String path = locUrl.substring(h1);
      String xml = httpGet(ip, path);
      PrinterHit hit; hit.ip = ip;
      xmlBetween(xml, "serialNumber", hit.serial);
      if (!hit.serial.length()) xmlBetween(xml, "SN", hit.serial);
      xmlBetween(xml, "friendlyName", hit.name);
      bool dup = false;
      for (auto &d : discovered) if (d.ip == hit.ip) dup = true;
      if (!dup && hit.serial.length()) { discovered.push_back(hit); blog("discovered: %s (%s)", hit.ip.c_str(), hit.serial.c_str()); }
    } else delay(50);
  }
  udp.stop();
  return discovered.size();
}

// ---------------- config ----------------
static void loadConfig() {
  prefs.begin("bsense", true);
  String ssid = prefs.getString("ssid", WIFI_SSID_DEFAULT);
  String pass = prefs.getString("pass", WIFI_PASS_DEFAULT);
  String name = prefs.getString("name", "bamboo-sense");
  prnIp     = prefs.getString("pip",  PRN_IP_DEFAULT);
  prnSerial = prefs.getString("pser", PRN_SERIAL_DEFAULT);
  prnCode   = prefs.getString("pcode",PRN_CODE_DEFAULT);
  prnUid    = prefs.getString("puid", PRN_UID_DEFAULT);
  prefs.end();
  if (ssid.length()) { WiFi.persistent(true); WiFi.begin(ssid.c_str(), pass.c_str()); }
  strlcpy(devName, name.c_str(), sizeof(devName));
}

// ---------------- camera ----------------
static bool camInit() {
  camera_config_t cfg = {};
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer = LEDC_TIMER_0;
  cfg.pin_d0 = PIN_CAM_Y2; cfg.pin_d1 = PIN_CAM_Y3;
  cfg.pin_d2 = PIN_CAM_Y4; cfg.pin_d3 = PIN_CAM_Y5;
  cfg.pin_d4 = PIN_CAM_Y6; cfg.pin_d5 = PIN_CAM_Y7;
  cfg.pin_d6 = PIN_CAM_Y8; cfg.pin_d7 = PIN_CAM_Y9;
  cfg.pin_xclk = PIN_CAM_XCLK; cfg.pin_pclk = PIN_CAM_PCLK;
  cfg.pin_vsync = PIN_CAM_VSYNC; cfg.pin_href = PIN_CAM_HREF;
  cfg.pin_sccb_sda = PIN_CAM_SIOD; cfg.pin_sccb_scl = PIN_CAM_SIOC;
  cfg.pin_pwdn = -1; cfg.pin_reset = -1;
  cfg.xclk_freq_hz = 10000000;
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.frame_size = FRAMESIZE_VGA;
  cfg.jpeg_quality = 12;
  cfg.fb_count = 2;
  cfg.fb_location = CAMERA_FB_IN_PSRAM;
  cfg.grab_mode = CAMERA_GRAB_LATEST;
  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) { blog("camera init FAIL 0x%x", err); return false; }
  blog("camera ready (VGA JPEG)");
  return true;
}

// ---------------- USB MSC ----------------
static int32_t mscRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  if (!sdOK) return -1;
  if (xSemaphoreTake(sdMtx, pdMS_TO_TICKS(4000)) != pdTRUE) return -1;
  int32_t done = -1;
  uint64_t byteAddr = (uint64_t)lba * 512 + offset;
  uint32_t sec = byteAddr / 512, off = byteAddr % 512;
  uint8_t *dst = (uint8_t *)buffer, tmp[512];
  uint32_t remaining = bufsize, copied = 0;
  if (off) {
    if (SD.readRAW(tmp, sec++)) { uint32_t n = 512 - off; if (n > remaining) n = remaining; memcpy(dst + copied, tmp + off, n); copied += n; remaining -= n; done = copied; }
    else { done = -1; remaining = 0; }
  }
  while (remaining >= 512 && done >= 0) { if (SD.readRAW(dst + copied, sec++)) { copied += 512; remaining -= 512; done = copied; } else done = -1; }
  if (remaining && done >= 0) { if (SD.readRAW(tmp, sec++)) { memcpy(dst + copied, tmp, remaining); copied += remaining; done = copied; } else done = -1; }
  xSemaphoreGive(sdMtx);
  return done;
}
static int32_t mscWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  if (!sdOK) return -1;
  if (xSemaphoreTake(sdMtx, pdMS_TO_TICKS(4000)) != pdTRUE) return -1;
  int32_t done = -1;
  uint64_t byteAddr = (uint64_t)lba * 512 + offset;
  uint32_t sec = byteAddr / 512, off = byteAddr % 512;
  uint8_t *src = (uint8_t *)buffer, tmp[512];
  uint32_t remaining = bufsize, written = 0;
  if (off) {
    if (SD.readRAW(tmp, sec)) { uint32_t n = 512 - off; if (n > remaining) n = remaining; memcpy(tmp + off, src, n);
      if (SD.writeRAW(tmp, sec++)) { written += n; remaining -= n; done = written; } else { done = -1; remaining = 0; } }
    else { done = -1; remaining = 0; }
  }
  while (remaining >= 512 && done >= 0) { if (SD.writeRAW(src + written, sec++)) { written += 512; remaining -= 512; done = written; } else done = -1; }
  if (remaining && done >= 0) { if (SD.readRAW(tmp, sec)) { memcpy(tmp, src + written, remaining); done = SD.writeRAW(tmp, sec) ? (int32_t)(written + remaining) : -1; } else done = -1; }
  xSemaphoreGive(sdMtx);
  return done;
}
static void usbRemount() {
  if (!sdOK) return;
  blog("usb: remount");
  tud_disconnect(); vTaskDelay(pdMS_TO_TICKS(900)); tud_connect();
}
static void sdInit() {
  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  sdOK = SD.begin(PIN_SD_CS, SPI, 25000000);
  if (!sdOK) { blog("SD: not present — MSC disabled"); return; }
  sdSectors = (uint32_t)(SD.totalBytes() / 512);
  blog("SD: %u sectors (%.1f GB)", sdSectors, SD.totalBytes() / 1073741824.0);
  msc.vendorID("SNAIL3D"); msc.productID("BAMBOOSENSE"); msc.productRevision("1.0");
  msc.onRead(mscRead); msc.onWrite(mscWrite); msc.mediaPresent(true);
  msc.begin(sdSectors, 512);
}

// ---------------- MQTT ----------------
static unsigned long long epochMs() {
  time_t now = time(nullptr);
  return now > 1600000000ULL ? (unsigned long long)now * 1000ULL : (unsigned long long)millis();
}

static void mqttCallback(char *topic, byte *payload, unsigned int length) {
  mqttMsgs++;
  std::string msg((const char *)payload, length > 24000 ? 24000 : length);
  bool isReport = strstr(topic, "/report") != nullptr;

  // ack correlation: scan every sequence_id occurrence (quoted or not); ours are >=100000
  size_t pos = 0;
  while ((pos = msg.find("\"sequence_id\"", pos)) != std::string::npos) {
    size_t p = msg.find(':', pos) + 1;
    while (p < msg.size() && (msg[p] == '"' || msg[p] == ' ')) p++;
    if (p < msg.size() && isdigit((unsigned char)msg[p])) {
      uint32_t s = (uint32_t)strtoul(msg.c_str() + p, nullptr, 10);
      if (s >= 100000) {
        bool hasResult = msg.find("\"result\"") != std::string::npos;  // result may precede sequence_id in the ack
        blog("ack-candidate seq=%u has-result=%s", s, hasResult ? "y" : "n");
        if (hasResult) {
          bool ok = msg.find("\"SUCCESS\"") != std::string::npos;      // whole-msg: fields are alphabetical
          ackSeq = s; ackErr = ok ? 0 : 1;
          snprintf(ackResult, sizeof(ackResult), "%s", ok ? "SUCCESS" : "FAIL");
          size_t ep = msg.find("\"err_code\":");
          ackErrCode = ep != std::string::npos ? strtol(msg.c_str() + ep + 11, nullptr, 10) : -1;
          break;
        }
      }
    }
    pos += 13;
  }

  if (isReport) {
    lastReport = msg;
    // mark install success: security reply for app_cert_install, or our id in app_cert_list
    if (msg.find("\"app_cert_install\"") != std::string::npos && msg.find("\"SUCCESS\"") != std::string::npos) {
      certInstalled = true;
      prefs.begin("bsense", false); prefs.putBool("certinst", true); prefs.end();
      blog("cert: install confirmed by printer");
    }
    if (msg.find("\"app_cert_list\"") != std::string::npos && idCertId.length()) {
      if (msg.find(idCertId.c_str()) != std::string::npos) {
        certInstalled = true;
        prefs.begin("bsense", false); prefs.putBool("certinst", true); prefs.end();
        blog("cert: confirmed installed on this printer");
      } else if (idCert.length() > 100) {
        blog("cert: NOT installed on this printer — queuing install");
        pendingCertInstall = true;              // per-printer trust: install wherever we're pointed
      }
    }
  }
}

static bool publishRaw(const String &json) {
  String reqTopic = "device/" + prnSerial + "/request";
  return mqtt.publish(reqTopic.c_str(), json.c_str());
}

// install our identity cert on the printer (unsigned command — the crack)
static void installCertOnPrinter() {
  if (idCert.length() < 100) { blog("cert: no identity provisioned — skip"); return; }
  blog("cert: installing identity on printer...");
  String crl = "-----BEGIN X509 CRL-----\nMIIB6jCB0wIBATANBgkqhkiG9w0BAQsFADAmMSQwIgYDVQQDDBtHTE9GMzgxMzcz\nNDA4OS5iYW1idWxhYi5jb20XDTI0MTIyODAzNDkyMloXDTI1MDEyODA0NDkyMlow\nSDAiAhEAwKnG9y7waEObyjeMvq6AsRcNMjQxMjIwMDczODQ0WjAiAhEA8Nae8vkm\nxTtjKidwSnzKexcNMjQxMjE5MDgwMjQ1WqAvMC0wHwYDVR0jBBgwFoAUwsnRLS7p\nktqZJwMZozos0xysCJswCgYDVR0UBAMCAXswDQYJKoZIhvcNAQELBQADggEBAIV+\njSqHblK7ZEH6eb8T7zFsFBPTrr4DKmwcBarCq9OLUtN/FSCcGXnVP6dWU06+RhE0\nmSwh6ER6LDQEupYXpOerZPE0zQOa5q/CsfTAtpBndMsKM9jKFTh0+Gr7V46fkuM\nkJ7UeO17FddDtfCDqxIvheo/RPvZPoiNuCpUQuGAI59O3kFqNkv6VsZlk+7E/D1Q\naSiKr+bk6+hWslSLtenA4rxZNcL8cq7AYijLPlE2HTN6ASCMx/bBZXzm28KHDyeR\nFtfnJsmWXBbeOqHmR9/JpSbJdXRD6jvXF2nQVgQcAqv3DZcOhov0ah+31foAe2/e\naRANWl5wMJ5nUd5UFCk=\n-----END X509 CRL-----\n";
  String body = "{\"security\":{\"sequence_id\":\"990001\",\"command\":\"app_cert_install\",\"timestamp\":" +
                String((unsigned long long)(epochMs())) + ",\"type\":\"app\",\"app_cert\":\"" + pemToJson(idCert) + "\",\"crl\":\"" + pemToJson(crl) + "\"}}";
  bool sent = publishRaw(body);
  blog("cert: install %s", sent ? "sent" : "PUBLISH FAIL");
  // verify after a beat
  vTaskDelay(pdMS_TO_TICKS(1500));
  String list = "{\"security\":{\"sequence_id\":\"990002\",\"command\":\"app_cert_list\",\"timestamp\":" +
                String((unsigned long long)(epochMs())) + ",\"type\":\"app\"}}";
  publishRaw(list);
}

static void mqttPoll() {
  static uint32_t lastTry = 0;
  if (!mqttEnabled) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) {
    mqtt.loop();
    if (pendingCertInstall) { pendingCertInstall = false; installCertOnPrinter(); }
    return;
  }
  if (millis() - lastTry < 5000) return;
  lastTry = millis();
  tlsClient.setInsecure();
  mqtt.setServer(prnIp.c_str(), 8883);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(32768);   // P2S reports are ~19KB
  mqtt.setKeepAlive(30);
  String cid = String(devName) + "-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX);
  bool ok = mqtt.connect(cid.c_str(), "bblp", prnCode.c_str());
  blog("mqtt %s connect %s (state=%d)", prnIp.c_str(), ok ? "OK" : "FAIL", mqtt.state());
  if (ok) {
    String repTopic = "device/" + prnSerial + "/report";
    bool subOk = mqtt.subscribe(repTopic.c_str());
    blog("mqtt subscribed %s: %s", repTopic.c_str(), subOk ? "OK" : "FAIL");
    lastReport.clear();
    // per-printer trust check: ask THIS printer which app certs it holds
    String list = "{\"security\":{\"sequence_id\":\"990003\",\"command\":\"app_cert_list\",\"timestamp\":" +
                  String((unsigned long long)(epochMs())) + ",\"type\":\"app\"}}";
    publishRaw(list);
    if (!certInstalled) installCertOnPrinter();
  }
}

// ---------------- status ----------------
static String jsonStatus() {
  char buf[900];
  double sdGb = sdOK ? SD.totalBytes() / 1073741824.0 : 0.0;
  snprintf(buf, sizeof(buf),
    "{\"name\":\"%s\",\"fw\":\"%s\",\"ip\":\"%s\",\"ap\":%s,"
    "\"heap\":%u,\"psram\":%u,\"uptime_s\":%lu,"
    "\"sd\":%s,\"sd_gb\":%.1f,\"usb_msc\":%s,"
    "\"mqtt\":\"%s\",\"mqtt_msgs\":%u,\"printer_cfg\":%s,\"camera\":%s,"
    "\"identity\":\"%.16s…\",\"cert_installed\":%s,\"uid\":\"%s\"}",
    devName, FW_VERSION, WiFi.localIP().toString().c_str(),
    (WiFi.getMode() & WIFI_MODE_AP) ? "true" : "false",
    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(), (unsigned long)(millis() / 1000),
    sdOK ? "true" : "false", sdGb, sdOK ? "true" : "false",
    mqttEnabled ? (mqtt.connected() ? "connected" : "connecting") : "disabled",
    mqttMsgs, (prnIp.length() ? "true" : "false"), camOk ? "true" : "false",
    idCertId.c_str(), certInstalled ? "true" : "false", prnUid.c_str());
  return String(buf);
}

// ---------------- dashboard ----------------
static const char DASHBOARD[] PROGMEM = R"HTML(<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>BambooSense</title><style>body{font-family:system-ui;margin:1rem;max-width:640px}fieldset{margin:.8rem 0}img{max-width:100%}</style></head><body>
<h2>&#128060; BambooSense v0.2 — signing vault</h2>
<p><a href=/snapshot>snap</a> &middot; <a href=/stream>stream</a> &middot; <a href=/status>status</a> &middot; <a href=/printer>printer</a> &middot; <a href=/discover>discover</a> &middot; <a href=/files>files</a> &middot; <a href=/logs>logs</a></p>
<img src="/snapshot" width=480><br><button onclick="location.reload()">refresh</button>
<fieldset><legend>Signed command (auto-signed print.*)</legend>
<input id=cmd style="width:80%%" value='{"command":"print_speed","param":"2"}'>
<button onclick="fetch('/cmd',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({print:JSON.parse(document.getElementById('cmd').value)})}).then(r=>r.text()).then(alert)">Send</button>
<button onclick="fetch('/light?set=on').then(r=>r.text()).then(alert)">Light on</button>
<button onclick="fetch('/light?set=off').then(r=>r.text()).then(alert)">Light off</button></fieldset>
<fieldset><legend>WiFi</legend><form method=POST action=/config>
SSID <input name=ssid> pass <input name=pass type=password> name <input name=name> <button>Save + reboot</button></form></fieldset>
<fieldset><legend>Printer</legend><form method=POST action=/printer>
IP <input name=ip> serial <input name=serial> access code <input name=code> user_id <input name=uid> <button>Save</button></form></fieldset>
<fieldset><legend>Firmware</legend>
<form method=POST action=/ota enctype=multipart/form-data><input type=file name=update required> <button>OTA flash</button></form>
<button onclick="fetch('/reboot',{method:'POST'}).then(()=>alert('rebooting'))">Reboot</button></fieldset>
<p><small>BambooSense — the steady pipe. Your printer, your keys.</small></p>
</body></html>)HTML";

static File uploadFile;

// ---------------- routes ----------------
static void setupRoutes() {
  server.on("/", HTTP_GET, []() {
    String p = DASHBOARD;
    server.send(200, "text/html", p);
  });

  server.on("/status", HTTP_GET, []() { server.send(200, "application/json", jsonStatus()); });

  server.on("/printer", HTTP_GET, []() {
    if (!mqttEnabled) { server.send(200, "application/json", "{\"mqtt\":\"disabled\"}"); return; }
    if (lastReport.size()) server.send(200, "application/json", lastReport.c_str());
    else server.send(200, "application/json", "{\"mqtt\":\"connected\",\"report\":\"none yet\"}");
  });

  // signed command relay: agent sends {"print":{...}} or a bare print object {...}
  // dongle injects a numeric sequence_id, signs, publishes, waits for the ack
  server.on("/cmd", HTTP_POST, []() {
    String body = server.arg("plain");
    if (!mqttEnabled || !mqtt.connected()) { server.send(503, "application/json", "{\"error\":\"mqtt not connected\"}"); return; }
    if (!body.length() || body.length() > 12000) { server.send(400, "application/json", "{\"error\":\"bad body\"}"); return; }

    JsonDocument doc;
    DeserializationError je = deserializeJson(doc, body);
    if (je) { server.send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
    JsonObject printObj = doc["print"].is<JsonObject>() ? doc["print"] : doc.as<JsonObject>();
    if (printObj.isNull() || !printObj["command"].is<const char*>()) {
      server.send(400, "application/json", "{\"error\":\"missing print.command\"}"); return;
    }
    printObj.remove("sequence_id");
    if (++cmdSeq > 890000) cmdSeq = 100001;
    uint32_t expect = cmdSeq;
    prefs.begin("bsense", false); prefs.putUInt("cmdseq", cmdSeq + 1); prefs.end();
    printObj["sequence_id"] = expect;
    String printJson;
    serializeJson(printObj, printJson);

    String wire;
    if (!signPrintCommand(printJson, wire)) { server.send(500, "application/json", "{\"error\":\"signing failed\"}"); return; }
    // debug: expose the full wire so the desktop can verify/replay byte-for-byte
    String innerDbg = wire;
    ackSeq = 0; ackErr = -1; ackResult[0] = 0;
    if (!publishRaw(wire)) { server.send(500, "application/json", "{\"error\":\"publish failed\"}"); return; }
    uint32_t t0 = millis();
    while (millis() - t0 < 2600) { mqtt.loop(); if (ackSeq == expect) break; delay(10); }
    bool dbg = server.hasArg("debug") && server.arg("debug") == "1";
    String resp;
    if (ackSeq == expect) {
      resp = String("{\"result\":\"") + ackResult + "\",\"err_code\":" + String(ackErrCode) +
             ",\"sequence_id\":" + String(expect) + "}";
    } else {
      resp = "{\"result\":\"timeout\",\"sequence_id\":" + String(expect) + "}";
    }
    if (dbg) { int h = resp.lastIndexOf('}'); resp = resp.substring(0, h) + ",\"wire\":\"" + innerDbg + "\"}"; }
    blog("cmd seq=%u -> %s", expect, ackSeq == expect ? ackResult : "timeout");
    server.send(200, "application/json", resp);
  });

  server.on("/light", HTTP_GET, []() {
    String state = server.arg("set");
    if (!mqttEnabled || !mqtt.connected()) { server.send(503, "text/plain", "mqtt not connected"); return; }
    if (state != "on" && state != "off") { server.send(400, "text/plain", "?set=on|off"); return; }
    String payload = "{\"system\":{\"command\":\"ledctrl\",\"led_node\":\"chamber_light\",\"led_mode\":\"" + state + "\",\"led_on_time\":500,\"led_off_time\":500,\"loop_times\":0,\"interval_time\":0}}";
    bool ok = publishRaw(payload);
    server.send(ok ? 200 : 500, "text/plain", "chamber light " + state + (ok ? "" : " (publish failed)"));
  });

  server.on("/discover", HTTP_GET, []() {
    int n = ssdpScan();
    String out = "{\"count\":" + String(n) + ",\"printers\":[";
    for (int i = 0; i < (int)discovered.size(); i++) {
      if (i) out += ",";
      out += "{\"ip\":\"" + discovered[i].ip + "\",\"serial\":\"" + discovered[i].serial + "\",\"name\":\"" + discovered[i].name + "\"}";
    }
    out += "]}";
    server.send(200, "application/json", out);
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
      File f; bool first = true;
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
    server.send(200, "text/plain", "uploaded — use /usb/rescan to rescan");
  }, []() {
    HTTPUpload &up = server.upload();
    static String fname;
    if (up.status == UPLOAD_FILE_START) {
      fname = (server.hasArg("filename") && server.arg("filename").length()) ? server.arg("filename") : up.filename;
      fname.replace("/", "_"); fname.replace("\\", "_"); fname.replace("..", "_");
      if (!fname.length()) fname = "upload.bin";
      if (sdOK) { tud_disconnect(); uploadFile = SD.open("/" + fname, FILE_WRITE); blog("upload start: %s", fname.c_str()); }
    } else if (up.status == UPLOAD_FILE_WRITE) {
      if (uploadFile) uploadFile.write(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END) {
      if (uploadFile) { blog("upload done: %s (%u bytes)", fname.c_str(), (unsigned)uploadFile.size()); uploadFile.close(); }
      if (sdOK) { vTaskDelay(pdMS_TO_TICKS(300)); tud_connect(); }
    } else if (up.status == UPLOAD_FILE_ABORTED) {
      if (uploadFile) uploadFile.close();
      if (sdOK) tud_connect();
    }
  });

  server.on("/usb/rescan", HTTP_POST, []() { usbRemount(); server.send(200, "text/plain", "ejected + replugged"); });

  server.on("/config", HTTP_POST, []() {
    prefs.begin("bsense", false);
    if (server.hasArg("ssid")) prefs.putString("ssid", server.arg("ssid"));
    if (server.hasArg("pass")) prefs.putString("pass", server.arg("pass"));
    if (server.hasArg("name") && server.arg("name").length()) prefs.putString("name", server.arg("name"));
    prefs.end();
    blog("config saved — rebooting");
    server.send(200, "text/plain", "saved, rebooting");
    rebootPending = true; rebootAt = millis() + 1200;
  });

  server.on("/printer", HTTP_POST, []() {
    if (server.hasArg("ip")) prnIp = server.arg("ip");
    if (server.hasArg("serial")) prnSerial = server.arg("serial");
    if (server.hasArg("code")) prnCode = server.arg("code");
    if (server.hasArg("uid") && server.arg("uid").length()) prnUid = server.arg("uid");
    prefs.begin("bsense", false);
    prefs.putString("pip", prnIp); prefs.putString("pser", prnSerial);
    prefs.putString("pcode", prnCode); prefs.putString("puid", prnUid);
    prefs.end();
    mqttEnabled = prnIp.length() && prnSerial.length() && prnCode.length();
    lastReport.clear(); mqtt.disconnect();
    server.send(200, "text/plain", "printer saved");
  });

  server.on("/identity", HTTP_GET, []() {
    String out = "{\"cert_id\":\"" + idCertId + "\",\"cert_installed\":" + String(certInstalled ? "true" : "false") +
                 ",\"provisioned\":" + String(idCert.length() > 100 ? "true" : "false") + "}";
    server.send(200, "application/json", out);
  });
  // provision identity: form args cert=<PEM-or-base64(PEM)>&key=<same>
  server.on("/identity", HTTP_POST, []() {
    auto fixPem = [](String &s) {
      s.replace("\r", "");
      s.trim();
      if (s.indexOf("-----BEGIN") < 0) {
        String t = s;
        bool isHex = t.length() > 200;
        for (size_t i = 0; i < t.length() && isHex; i++) {
          char c = t[i];
          if (!isxdigit((unsigned char)c)) isHex = false;
        }
        unsigned char *out = (unsigned char *)malloc(s.length() / 2 + 16);
        size_t olen = 0;
        if (out && isHex) {
          // hex transport (bulletproof vs any urlencoding)
          for (size_t i = 0; i + 1 < t.length(); i += 2) {
            char byte[3] = {t[i], t[i + 1], 0};
            out[olen++] = (unsigned char)strtoul(byte, nullptr, 16);
          }
          out[olen] = 0;
          s = String((const char *)out);
        } else {
          // base64 fallback
          while (t.length() % 4) t += "=";
          t.replace(" ", "+");   // urlencoded '+' arrives as space — base64 never has real spaces
          unsigned char *b64out = (unsigned char *)malloc(t.length() + 16);  // decoded ≤ 3/4·len, but be generous
          size_t blen = 0;
          if (b64out && mbedtls_base64_decode(b64out, t.length() + 8, &blen, (const unsigned char *)t.c_str(), t.length()) == 0) {
            b64out[blen] = 0;
            s = String((const char *)b64out);
          }
          free(b64out);
        }
        free(out);
      }
      if (s.indexOf("\\n") >= 0) s.replace("\\n", "\n");
      s.trim();
      if (s.indexOf("-----BEGIN") >= 0 && !s.endsWith("\n")) s += "\n";  // ESP32 x509 PEM parser requires trailing newline
    };
    String cert = server.arg("cert"), key = server.arg("key");
    fixPem(cert); fixPem(key);
    if (cert.length() < 100 || key.length() < 100 || cert.indexOf("-----BEGIN") < 0) {
      String dbg = "received: cert_len=" + String(cert.length()) + " key_len=" + String(key.length()) +
                   " cert_head=" + cert.substring(0, 40) + " cert_tail=" + cert.substring(cert.length() > 40 ? cert.length() - 20 : 0);
      server.send(400, "text/plain", dbg); return;
    }
    if (provisionIdentity(cert, key)) { mqtt.disconnect(); server.send(200, "text/plain", "identity provisioned — reinstalling on printer"); }
    else {
      // sha256 of received bytes for byte-level comparison with the sender
      unsigned char sha[32];
      mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const unsigned char *)cert.c_str(), cert.length(), sha);
      char shaHex[65];
      for (int i = 0; i < 32; i++) snprintf(shaHex + i * 2, 3, "%02x", sha[i]);
      String dbg = "parse fail: cert_len=" + String(cert.length()) + " sha256=" + String(shaHex) +
                   " nl=" + String(std::count(cert.begin(), cert.end(), '\n'));
      server.send(400, "text/plain", dbg);
    }
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

  server.onNotFound([]() { server.send(404, "text/plain", "nope"); });
}

// ---------------- setup/loop ----------------
void setup() {
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.begin(115200);
  uint32_t t0 = millis(); while (!Serial && millis() - t0 < 1500) delay(10);
#endif
  blog("BambooSense v%s (signing vault) booting", FW_VERSION);

  sdMtx = xSemaphoreCreateMutex();
  loadConfig();
  loadIdentity();
  certSelfTest();
  if (idCert.length() < 100 || idKey.length() < 100) blog("crypto: NO identity — POST /identity (cert+key PEMs) to enable signing");
  else blog("crypto: identity loaded (%s…)", idCertId.c_str());

  { prefs.begin("bsense", true); uint32_t saved = prefs.getUInt("cmdseq", 100001); prefs.end(); if (saved > cmdSeq) cmdSeq = saved; }
  camOk = camInit();
  sdInit();
  USB.begin();

  if (WiFi.status() != WL_CONNECTED) {
    blog("wifi: connecting...");
    uint32_t t1 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t1 < 20000) delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    blog("wifi: connected %s (%s)", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    configTime(0, 0, "pool.ntp.org");   // epoch for security command timestamps
    if (MDNS.begin(devName)) { MDNS.addService("http", "tcp", 80); blog("mdns: %s.local", devName); }
    // zero-touch: if unbound, discover printers; auto-bind when exactly one
    if (!prnIp.length()) {
      int n = ssdpScan();
      if (n == 1) {
        prnIp = discovered[0].ip; prnSerial = discovered[0].serial;
        prefs.begin("bsense", false);
        prefs.putString("pip", prnIp); prefs.putString("pser", prnSerial);
        prefs.end();
        blog("auto-bound to %s (%s) — access code still needed for commands", prnIp.c_str(), prnSerial.c_str());
      } else {
        blog("discovery: %d printer(s) — pick via /discover + POST /printer", n);
      }
    }
  } else {
    uint16_t tag = (uint32_t)(ESP.getEfuseMac() >> 32) & 0xFFFF;
    char ap[40]; snprintf(ap, sizeof(ap), "%s-%04X", devName, tag);
    WiFi.mode(WIFI_AP); WiFi.softAP(ap);
    blog("wifi: FAILED — portal AP up: %s  http://192.168.4.1/", ap);
    strlcpy(devName, ap, sizeof(devName));
  }

  mqttEnabled = prnIp.length() && prnSerial.length() && prnCode.length();
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
