# BambooSense 🐼

**A $10 dongle that gives Bambu Lab printers a steady, agentic pipe** — files, commands, and eyes — without LAN-only mode, without touching Bambu's app, and without caring when Bambu changes their cloud.

Built on the Seeed Studio **XIAO ESP32-S3 Sense** (ESP32-S3 + OV2640 camera + microSD slot). It plugs straight into the printer's USB-C/USB port. The printer sees a normal USB flash drive. Your agent sees everything else.

```
 agent (pi / Mac / MCP)
   │  WiFi / Tailscale
   ▼
┌──────────────────────────┐        USB-C into printer USB port
│  BambooSense dongle      │────────────────────────────────┐
│  · USB MSC (fake stick)  │      printer mounts it like    │
│  · OV2640 camera → /snap │      a normal U-disk           │
│  · MQTT client (bblp)    │──────────────► Bambu printer   │
│  · FTPS relay (X.509)*   │        8883 monitor/control    │
└──────────────────────────┘        + 990 file upload*     │
                                     + camera down through │
                                     the top glass 📷      │
```
*FTPS/X.509 relay = phase 2 (see RESEARCH.md — the extracted Bambu Connect cert makes this work in cloud mode).

## Why

Bambu's official paths for agents are noisy: cloud API changes, the app's UI churn, carrier blocks on 8883, Jan-2025 firmware auth lockdown (later cracked with the extracted X.509 cert). BambooSense is the **steady pipe**: one cable for power + USB storage, one WiFi connection for control, its own camera that belongs to *us*, not to Bambu's firmware roadmap.

## What it does (v0.1)

| Piece | Detail |
|---|---|
| **Fake USB stick** | TinyUSB MSC backed by the microSD card. Printer sees a U-disk. Push a sliced `.3mf` over HTTP → dongle remounts the drive → file appears on the printer. |
| **Bed camera** | OV2640 pointed down through the top glass. `GET /snapshot` for the agent to judge *bed clear / print failed / spaghetti*. |
| **MQTT bridge** | Dongle joins the printer's LAN broker (8883, `bblp` + access code) in **cloud mode** — status, progress, layer, pause/resume/stop, lights. Cached and served as JSON. |
| **OTA** | `POST /ota` pushes new firmware over WiFi. After the first USB flash you never open the case again. |
| **Zero-config setup** | No WiFi stored → it broadcasts its own AP; the Pi (or Mac) joins it and hands it credentials over HTTP. |

## Endpoints

| Endpoint | What |
|---|---|
| `GET /` | dashboard |
| `GET /status` | dongle JSON (heap, uptime, SD, USB, MQTT state) |
| `GET /printer` | last MQTT report from the printer (raw JSON) |
| `GET /snapshot` | JPEG frame of the bed |
| `GET /stream` | MJPEG live stream |
| `GET /files` | list files on the fake stick |
| `POST /upload?filename=x.3mf` | push a file (multipart) |
| `POST /usb/rescan` | eject + remount so the printer re-reads the stick |
| `POST /config` | set WiFi (form: `ssid`, `pass`, `name`) |
| `POST /printer` | set printer creds (form: `ip`, `serial`, `code`) |
| `POST /ota` | firmware upload (multipart field `update`) |
| `GET /logs` | recent device log |
| `POST /reboot` | reboot |

## Build & flash

```bash
cd firmware
pio run -e bench        # bench build: USB serial + MSC (for desk/Pi testing)
pio run -e printer      # printer build: MSC-only (cleanest for the printer's USB host)
pio run -e bench -t upload --upload-port /dev/cu.usbmodem*   # first flash over USB
tools/ota_push.sh bamboo-sense.local .pio/build/printer/firmware.bin   # every flash after
```

First boot with no WiFi: it starts AP `bamboo-sense-XXXX`. From any laptop (or the Pi's WiFi dongle):

```bash
tools/provision_ap.sh bamboo-sense-XXXX "<home wifi ssid>" "<wifi password>"
```

## Agentic check-in plan (the fun part)

1. Submit print (cloud/app or phase-2 FTPS relay from the dongle).
2. Agent subscribes to `/printer` progress + polls `/snapshot`.
3. **Check at t+3min** (first layer = make or break), then at layer milestones, after high-risk features (bridges/overhangs from the slicer metadata), and on any progress stall.
4. Camera says spaghetti? Agent stops the print over MQTT and tells you.

Roadmap: FTPS upload relay (X.509) → MQTT `project_file` start-from-SD → risky-layer scheduling → one dongle per printer, fleet view → MCP server so any agent can drive it.

See `docs/RESEARCH.md` for the full state-of-the-union on controlling Bambu printers in 2025+.
