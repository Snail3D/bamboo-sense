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
