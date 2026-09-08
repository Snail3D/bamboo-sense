# The Bambu Signing Crack — working recipe (verified 2026-09-07 on P2S, cloud mode)

Bambu's post-2025 "local security" turned out to be **trust-on-install**:
the printer verifies `print.*` MQTT command signatures against certificates
*it has installed itself* — via an unsigned `security.app_cert_install`
command. We minted our own CA-issued identity and the printer accepted it.

## Working sequence

1. **Mint any RSA-2048 cert+key** (self-signed works — the printer never
   validates the chain, expiry, or issuer):
   ```
   openssl req -new -x509 -key mykey.pem -out mycert.pem -days 3650 \
     -subj "/CN=GLOF3813734089.bambulab.com" -set_serial 0x5a5a...
   ```
2. **Install it on the printer** over plain MQTT (bblp + access code, port 8883,
   NO client cert needed):
   ```json
   {"security":{"sequence_id":"1","command":"app_cert_install",
    "timestamp":<ms>,"type":"app",
    "app_cert":"-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n",
    "crl":"-----BEGIN X509 CRL-----\n...\n-----END X509 CRL-----\n"}}
   ```
   Reply comes back with `printer_cert` (the device's own cert — keep it).
   Confirm with `app_cert_list` → our serial appears.
3. **Sign every `print.*` command**:
   - `cert_id` = lowercase hex of the cert serial (32 chars) + `CN=<subject CN>`
   - bytes-to-sign = `{"print":{...compact json...},"user_id":"<uid>"}`
     * insertion order: `print` FIRST, then `user_id`
     * **`sequence_id` must be a NUMBER** (not a string)
     * compact separators, no spaces
   - header = `{"sign_ver":"v1.0","sign_alg":"RSA_SHA256","sign_string":<b64 RSA-SHA256 PKCS1v15>,"cert_id":...,"payload_len":<len(bytes)>}`
   - wire = signed-bytes with `,"header":{...}` spliced before the final `}`
4. Publish to `device/<serial>/request`. Printer answers `result: SUCCESS`.

## Error-code map (learned by probing)
| err_code | meaning |
|---|---|
| 0x05024007 (84033543) | no header at all |
| 0x0502400A (84033546) | header present, signature not understood (string-sequence_id / wrong canonical form) |
| 0x0502400B (84033547) | envelope shape right, signature bytes wrong |
| 0x05024007 again with valid-shape envelope | cert not installed / cert_id mismatch |

## The hunt (how we got here)
- OpenBambuAPI `cloud-x509-auth.md` gave the scheme but a WRONG canonicalization
  (sorted-keys recipe; real firmware wants insertion-order + trailing user_id).
- Decompiled Bambu Connect (`Randomblock1/bambu_connect_disasm`) revealed the true
  flow: sign `JSON.stringify(envelope-without-header)`, `payload_len` = its UTF-8
  length. The `s1()` signer is a native bridge, so the exact bytes were recovered
  by variant-testing against the printer's error codes.
- Bambu Studio binary (libbambu_networking.dylib) contains an encrypted embedded
  app-cert bundle + pinned CA store (TLS interception resisted — pinned CAs even
  for LAN brokers), but none of that matters: you can bring your own cert.
- FTPS (990) still gates STOR on a client cert; with our cert installed, login
  works, LIST works; writes returned 553 on a paused/busy printer (retest idle).

## What this unlocks
Signed `print.*` = pause/resume/stop/speed AND `project_file` (start prints),
`gcode_line` (live G-code, needs the extra param_enc step + printer_cert),
AMS control, firmware-adjacent ops — all over LAN, cloud mode, no LAN-only mode,
no Bambu account dependency at runtime. BambooSense dongle = the signing vault.

---

# PROJECT_FILE: starting actual prints (2026-09-08, P2S fw live-verified)

The full start-a-print chain, every step verified on P3Pio (P2S, cloud mode):

## 1. Command shape
```json
{"print":{"sequence_id":<int>,"command":"project_file","param":"Metadata/plate_1.gcode",
  "url":"<URL>","md5":"<md5-of-THE-ENTIRE-3MF-FILE>","project_id":"0","profile_id":"0",
  "task_id":"0","subtask_id":"0","use_ams":true,"ams_mapping":[<tray>],
  "bed_leveling":true,"flow_cali":false,"vibration_cali":true,"layer_inspect":true,
  "timelapse":false},"user_id":"<uid>"}
```
signed exactly like any print.* (header spliced before final `}` — see above).

- `md5` = **md5 of the whole .3mf file**, NOT the `Metadata/plate_1.gcode.md5`
  inside it. Wrong md5 ⇒ task dies in PREPARE with print_error 83902527
  (0x0500403F), before any heating.
- `ams_mapping` = list of AMS tray indices (0-3), one per filament in the plate.
  `use_ams:false` ⇒ printer expects filament on the external spool holder; if
  empty it fails mid-PREPARE with HMS_0300-0200 (filament ran out).
- `url` works as **plaintext** — `url_enc` (RSA to printer_cert) is accepted but
  NOT required on this firmware.

## 2. THE BIG ONE: url can be a plain LAN http:// URL (cloud mode!)
Cloud-mode (non-LAN-only) P2S happily fetches `http://<lan-ip>:<port>/file.3mf`.
Verified: 18.7 MB sliced 3mf served by `python3 -m http.server`, printer did
HEAD then GET, download ~seconds, print started, first layers went down.
**This means: fully-local print start on a cloud-mode printer — no Bambu cloud
file service, no LAN-only mode, no SD card.** A dongle that hosts the file over
HTTP + signs the command = complete local pipeline.

## 3. The trap: presigned S3 URLs are REJECTED
`url = https://s3.../or-cloud-upload-prod/...?AWSAccessKeyId=..&Signature=..`
(upload via cloud API `v1/iot-service/api/user/upload`, PUT 200 OK) ⇒ printer
ACCEPTS the command (ACK SUCCESS), shows PREPARE, then fails ~75 s later with
print_error 83902527, no heating, HMS_0100-0100 logged. Reproduced 3×.
Hypothesis: firmware only fetches from allow-listed Bambu hosts (or https-only).
If you see "ACK SUCCESS → PREPARE → FAILED 0x0500403F": it's the URL, not your
signature, not the md5, not the AMS.

## 4. State machine gotchas
- `gcode_state` FINISH accepts new project_file. FAILED does NOT — any new
  project_file ⇒ `{"result":"FAIL","reason":"ERROR STATE"}`.
- The FAILED latch (incl. err print_error display) is only clearable on the
  printer touchscreen (OK) or power cycle. Signed `stop` twice / `resume` do
  NOT clear it. `stop` from FINISH is a no-op but returns SUCCESS.
- Filament-runout FAIL (use_ams:false + empty external spool) also latches FAILED.
- Signed `stop` during RUNNING works instantly (verified at 238 °C nozzle).

## 5. Slicing for P2S via CLI (OrcaSlicer, no GUI)
BambuStudio AND OrcaSlicer CLI **segfault** slicing P2S profiles out of the box:
`extruder_variant_list: ["Direct Drive Standard,Direct Drive High Flow"]` (two
comma-separated variants for ONE extruder) trips support_different_extruders()
→ multi-extruder path → crash. Fix: collapse to a single variant in the machine
profile. Also required: `from:"system"` (not "user") in every loaded profile,
`compatible_printers` matching, `nozzle_volume_type` present.
Working invocation:
```
OrcaSlicer --orient 1 --arrange 1 \
  --load-settings "process.json;machine.json" --load-filaments "filament.json" \
  --slice 0 --export-3mf out.gcode.3mf model.stl
```

## 6. Clearing errors remotely: `clean_print_error` (undocumented)
`{"print":{"command":"clean_print_error","sequence_id":N}}` — signed like any
print.* — is ACCEPTED by 2026 P2S firmware and **clears the HMS alarm list**
(verified: HMS entry 0x10001 vanished after firing it). The FAILED-latched
screen dialog: `gcode_state` stays FAILED as a sticky last-task-outcome field
(same way FINISH persists) — the dialog and the state field are different
things. Prints can START even while gcode_state reads FAILED (LAN-url
project_file did — the earlier "ERROR STATE" refusals were tied to the S3-url
attempts, not to the latch). `system.restart`/`reboot` over MQTT: silently
ignored. If a modal truly remains on-screen, touch OK or power cycle.

## 7. app_cert_install gotcha: CRL issuer MUST match cert issuer (2026-09-08)
Second dongle's cert was self-signed `CN=bamboo-sense2` while the CRL (baked
into firmware + manual attempts) was issued by `CN=GLOF3813734089.bambulab.com`.
Printer **silently dropped** every install (no security reply at all — worse
than the explicit `FAILURE no crl` you get when omitting the CRL entirely).
Fix: sign the dongle's cert WITH the GLOF CA key (the self-made issuer key),
then the same known-good GLOF CRL validates and install confirms instantly
("install confirmed by printer", cert_installed: true). Rule: issuer(cert) ==
issuer(crl) or radio silence. Also: omitting CRL explicitly returns
`{"result":"FAILURE","reason":"no crl"}` — so a REPLY means the CRL parsed, and
SILENCE means it didn't. Useful error triage.

## 8. P2S (N7-V2) TLS service map (all mTLS, same server cert CN=<serial>,
##    issuer "BBL Device CA N7-V2")
- :8883 MQTT (bblp/access code) — the main door (see above)
- :990 FTPS — LIST works with creds; STOR 553 (still)
- :6000 file tunnel — 64-byte auth ("bblp"+code) → 24b frame; LIST framing TBD
- :322 RTSPS — RTSP/1.0 answers WITHOUT client cert (Basic auth bblp:code);
  all stream paths 404 on 01.02.00.00 (RTSP server vestigial — video goes via
  their "brtc" protocol, see ipcam.brtc_service in reports)
- :3002 mystery TLS — silent to RTSP/HTTP/tunnel-auth; likely brtc media
- :3000 TLS — silent to HTTP + tunnel-auth
Signed print.* now VERIFIED on both printers (P3Pio via dongle1, P2D2 via
dongle2 after issuer fix + printer power-cycle + fw update).
