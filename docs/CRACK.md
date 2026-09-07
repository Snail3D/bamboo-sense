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
