# BambooSense research notes — state of Bambu control, 2025+

Compiled from GitHub + the web (SearXNG) on project kickoff.

## The landscape

### The Jan-2025 lockdown, and the crack
- Bambu pushed firmware (Jan 2025) requiring authentication for local LAN control — broke OctoPrint, HA, custom tools overnight.
- Community extracted the **X.509 client cert + private key from Bambu Connect** (the desktop app), restoring third-party FTP/camera/MQTT-signing access even in **cloud mode**:
  - Hackaday: "Bambu Connect's Authentication X.509 Certificate and Private Key Extracted"
  - **OpenBambuAPI** (github.com/Doridian/OpenBambuAPI) — reverse-engineered MQTT/HTTP/FTP protocol docs, ships the cert.
- Practical result (verified by multiple projects): in cloud mode you can still do **everything over LAN** if you speak MQTT (8883) + FTPS (990) with the right creds/cert. No LAN-only mode needed.

### Print-job submission without the app (the big one)
- MQTT `project_file` with `url: "file:///sdcard/<path>"` starts a print from the **printer's internal storage** — confirmed by `davglass/bambu-cli` (README TODO: "Trigger a print job from a file on the sdcard"), the Home Assistant Bambu MQTT thread (#738: "there's a project_file command that will let you start a print remotely using a path on the SD card"), and OrcaSlicer issue #9254 (mentions `file:///sdcard/{filepath}` error semantics).
- schwarztim/bambu-mcp: **`.gcode` via project_file works WITHOUT Developer Mode**; `.3mf` via project_file requires Developer Mode (Settings → LAN Only → Developer Mode). FTPS upload (port 990) puts the file on internal storage first.
- DMontgomery40/bambu-printer-mcp: verified end-to-end path = FTPS upload + MQTT `project_file` + AMS mapping built from the 3MF metadata.
- Our own fleet notes said "LAN FTP write returns 553" — that matches the **missing X.509 client cert on post-Jan-2025 firmware**, not a hard block. Retest with the cert.

### Prior art worth stealing from
| Project | What it gives us |
|---|---|
| `schwarztim/bambu-mcp` | MQTT + FTPS + X.509 + camera in one MCP server (Node). Command/JSON reference impl. |
| `DMontgomery40/bambu-printer-mcp` | FTPS upload + project_file + AMS mapping recipe, docs/SLICING.md. |
| `Doridian/OpenBambuAPI` | Protocol docs + the X.509 cert/key files. |
| `davglass/bambu-cli` (archived) | sdcard print trigger payload shapes; cloud-login breakage history. |
| HA community Bambu MQTT thread | project_file from SD card payloads incl. AMS mapping. |
| `esp-cpp/esp-box-emu` | TinyUSB MSC on ESP32-S3 in production (pattern for our fake stick). |

### What does NOT exist (our gap)
- **Nobody has built the ESP32 dongle**: a single USB-plugged device that is simultaneously (a) the printer's USB stick, (b) the MQTT/FTPS bridge, (c) an independent bed camera, and (d) OTA-updatable. That's BambooSense. Good YouTube.

## LIVE FINDINGS — P2S on 2026 firmware (probed 2026-09-06, dongle + Python)

Empirically verified against P2D2 (P2S, fw with vsftpd 3.0.5, serial CN certs):

| Layer | Result |
|---|---|
| MQTT connect (no client cert) | ✅ accepted — TLS handshake requests a cert but proceeds without one |
| MQTT subscribe `/report` | ✅ works — full ~19KB telemetry every second |
| MQTT publish `system.*` (ledctrl) | ✅ **works unsigned** — chamber light responds |
| MQTT publish `print.*` (print_speed) | ❌ `result:failed, reason:"mqtt message verify failed", err_code:0x05024007` — **signed envelope required** |
| FTPS :990 login (bblp+code, no cert) | ✅ login OK, PBSZ/PROT P OK |
| FTPS write (STOR anywhere) | ❌ `553 Could not create file` — server sends TLS `Request CERT`; writes gated on client cert |
| FTPS read/list | root LIST returns empty (restricted session) |

### The signed-envelope scheme (OpenBambuAPI cloud-x509-auth.md)
- `print.*` commands need a `header` envelope: RSA-SHA256 over canonical JSON (`sort_keys`, no whitespace, wrapped as `{"print":...}`), base64 `sign_string` + `cert_id` (leaf serial + CN) + `payload_len`.
- Requires a **per-printer client cert+key** (CN = printer serial, chains to BBL CA, ~10yr validity). Also needed for FTPS writes and `gcode_line`'s extra `param_enc` (RSA-encrypt gcode with the printer's own pubkey from `cert_report`).
- Cloud minting endpoint exists (`GET /v1/iot-service/api/user/applications/{appToken}/cert?aes256=...&ver=1`) but the appToken/AES-256 payload construction is proprietary — found in decompiled Bambu Connect (`Randomblock1/bambu_connect_disasm`, function wrapping `ac1()` → `{encAppKey, aes256}`), not yet replicated publicly.
- Documented extraction: scan a **Linux** Bambu Studio network-plugin process memory for PEM markers after it connects to the printer.
- Bambu Studio on this Mac: logs decrypt with the fixed plugin key `yyuBcftO2jkZeucy` (AES-128-ECB, `debug_network_*.log.enc`) — plugin v02.05.00.56, but no cert material on disk or in logs. macOS blocks process-memory scanning (SIP).

### Bottom line for BambooSense
- **Today, cert-free:** monitoring, lights, camera, OTA — done and running. Job submission via the **cloud API** (proven in fleet ops) fills the gap.
- **Full local control needs the per-printer cert+key.** Three routes: (1) Linux Studio memory extraction (Docker x86_64 on Mac or Nick's PC), (2) replicate the Connect cert-minting crypto, (3) community-shared cert for our serial (none public — certs are per-printer).

## Firmware facts locked in during scaffold
- Board: Seeed XIAO ESP32-S3 Sense — ESP32-S3R8 (8MB OPI PSRAM), 8MB flash, OV2640 on the expansion board, microSD on SPI.
- Camera pins (Seeed wiki): XCLK 10, SIOD 40, SIOC 39, D0..D7 = 15,17,18,16,14,12,11,48, VSYNC 38, HREF 47, PCLK 13. microSD: CS 21, SCK 7, MOSI 8, MISO 9. Onboard LED shares GPIO21 with SD CS (flickers with SD traffic — cosmetic).
- TinyUSB MSC lives in arduino-esp32 core (`USB.h` + `USBMSC.h`); MSC needs `ARDUINO_USB_MODE=0`; SD SPI exposes per-sector `readRAW`/`writeRAW` — enough to back an MSC LUN with sector-multiple transfers.
- Printer USB port must supply ~500mA; XIAO S3 + camera + WiFi TX bursts fit if camera is on-demand only. If marginal: cap camera FPS, lower WiFi TX power.
- Printer LAN surface in cloud mode: 8883 MQTT (works today, fleet-verified), 990 FTPS (needs X.509 client cert), camera stream 322 (P2 series) — the dongle's own camera sidesteps all of it.

## Sources
- https://github.com/schwarztim/bambu-mcp
- https://github.com/DMontgomery40/bambu-printer-mcp
- https://github.com/Doridian/OpenBambuAPI
- https://github.com/davglass/bambu-cli (archived 2025-01-28)
- https://community.home-assistant.io/t/bambu-lab-x1-x1c-mqtt/489510/738
- https://github.com/OrcaSlicer/OrcaSlicer/issues/9254
- https://wiki.bambulab.com/en/general/printer-network-ports
- https://hackaday.com/2025/01/19/bambu-connects-authentication-x509-certificate-and-private-key-extracted/
