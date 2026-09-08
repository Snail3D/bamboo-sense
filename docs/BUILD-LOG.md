# BambooSense build log

## 2026-09-06 — v0.1.0 → v0.1.1 (first full pipe)

### What happened
1. Flashed v0.1.0 from **snailpi** (Raspberry Pi 5, 192.168.1.92, user `snailpi`) with esptool 5.3.1 → `/dev/ttyACM0` (Espressif USB JTAG/serial). 1MB firmware, hash verified.
2. Dongle joined SpectrumSetup-617D → 192.168.1.236, mDNS `bamboo-sense.local`.
3. MQTT connected to P2D2 (192.168.1.81:8883, bblp + access code) in **cloud mode** — no LAN-only change.
4. **Bug found**: P2S heartbeat reports are **~19,040 bytes**; PubSubClient buffer was 16KB → zero messages parsed, periodic disconnects. Python paho side-test confirmed 21 msgs/25s with identical creds. Fix: buffer 32KB.
5. **v0.1.1 pushed over OTA from the Mac** (tools/ota_push.sh → POST /ota multipart). Worked first try.
6. Verified: reports flowing, `/printer` shows live state (PAUSE, spd_lvl 2), `/light?set=on` chamber-light command accepted by the printer.

### Flash-from-Pi recipe (for future dongles)
```bash
scp firmware/{bootloader,partitions,boot_app0,firmware}.bin snailpi@192.168.1.92:~/bs-flash/
ssh snailpi@192.168.1.92
esptool --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
  --before default-reset --after hard-reset write-flash -z \
  --flash-mode dio --flash-freq 80m --flash-size 8MB \
  0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
```
esptool v5 uses dash-style flags (`write-flash`, `default-reset`), NOT underscores.

### Gotchas learned
- **P2S reports ~19KB** — any MQTT client buffer must be ≥32KB (PubSubClient `setBufferSize(32768)`).
- XIAO S3 Sense: onboard LED shares GPIO21 with SD CS → flickers during SD traffic (cosmetic).
- Printer build has no USB CDC → logs only via `GET /logs` ring buffer.
- mDNS on this network is flaky from the Mac; use the IP (DHCP reservation candidate: 192.168.1.236).
- esptool on Pi 5 with the XIAO in a **host** USB port works fine at 921600.

### Still to do
- ~~Insert microSD~~ — retired: no SD needed; files go over the wire (signed FTPS/project_file path).
- ~~Move dongle: Pi USB → printer front USB-C~~ — **DONE 2026-09-06**: dongle lives in the printer's USB port, powered by the printer, MQTT live from inside the chamber, light control verified through the dongle. Camera mounted top-down on the glass (position tuneable).
- ~~Signed print.* commands~~ — **DONE, verified**: `/cmd` → printer acks SUCCESS (v0.2.0, see CRACK.md).
- Next: signed `project_file` (start prints through the dongle), FTPS write retest with our cert installed, check-in cadence loop (t+3min, risky layers, stall detection).

---

## 2026-09-07/08 — THE PIPE GOES LIVE: first real prints started through the dongle

### Session outcome (all live-verified on P3Pio, cloud mode)
1. **project_file fully cracked** (see CRACK.md §1-5): signed `project_file` with
   **plain LAN http url** + **full-file md5** + `ams_mapping:[tray]` starts real
   prints. Fired through the dongle `/cmd` (on-chip RSA signing). Statue test print
   downloaded 18.7MB from the Mac's http.server and reached first layers;
   stopped cleanly via signed `stop` on Eric's request.
2. **Killer negative result**: cloud presigned S3 URLs are REJECTED in PREPARE
   (print_error 0x0500403F ×3, no heating). LAN http urls work. ⇒ fully-local
   print start on a cloud-mode printer.
3. **Undocumented `clean_print_error`** print command discovered — accepted,
   clears HMS alarm list. `system.restart`/`reboot` silently ignored.
4. **OrcaSlicer CLI P2S segfault root cause** posted upstream
   (bambulab/BambuStudio#11893 comment): dual-variant `extruder_variant_list`
   trips the multi-extruder path. Collapse to one variant → exit 0.
5. **FIND → SLICE → PRINT demo** (Eric: "find me a fidget and print it"):
   - Found DrLex0/print3D-FlexiRex via GitHub search (direct STL, open license)
   - Sliced with the proven P2S profiles (0.16mm, PLA, no supports, flat)
   - Fired via `tools/print_file.sh` path → **Flexi Rex RUNNING on P3Pio**
     (81 layers, ~50 min, AMS tray 1)
6. Dongle held through everything: MQTT inside the chamber, ~6h uptime,
   reports + camera + light + signed commands all live.

### New tool: tools/print_file.sh
One-shot: serves the dir on :8477, computes full-file md5, builds the signed
project_file (via dongle /cmd), watches until RUNNING. First real print:
`tools/print_file.sh /tmp/bswork/flexirex.gcode.3mf 1`.

### Upstream contributions (2026-09-08)
- Doridian/OpenBambuAPI#72 — signing canonicalization corrections + project_file recipe
- coelacant1/Bambu-Lab-Cloud-API#17 — upload_file never registers; presigned urls rejected by printer
- bambulab/BambuStudio#11893 (comment) — CLI segfault root cause + workaround

### Camera
Still bed-level (sees plate edge-on + parts bottom-center). Needs a reposition
mount on the glass frame for a real top-down view. Bed-check rule stands:
eyeball via /snapshot before firing prints.

### Still to do
- Move camera to top-down mount
- checkin.sh cadence loop around live prints (t+3min, stall detect)
- FTPS write retest now that cert is installed (probably still 553; low priority
  since project_file+http covers everything)
- port-6000 file tunnel: retired for file transfer (http url won), keep for reference
