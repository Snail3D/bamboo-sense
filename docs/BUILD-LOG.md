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
