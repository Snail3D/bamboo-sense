#!/bin/bash
# print_file.sh — slice nothing, just FIRE a pre-sliced .gcode.3mf at a printer
# through the BambooSense dongle (signed project_file over LAN).
#
# Usage: print_file.sh <file.gcode.3mf> [ams_tray_index] [printer_ip] [dongle_ip]
#   ams_tray  : AMS tray index 0-3 (default 1; empty box = use_ams:false path)
#   printer_ip: default 192.168.1.154 (P3Pio)
#   dongle_ip : default 192.168.1.236
#
# Requirements:
#   - dongle online with cert installed (fw >= 0.2.2)
#   - an HTTP server serving the file dir on :8477 (script starts one if missing)
#   - printer state FINISH or IDLE (FAILED latch: tap screen or power cycle;
#     NOTE LAN-url project_file often starts anyway)
#
# Recipe (verified live, see docs/CRACK.md):
#   url = plain LAN http://<this-mac>:8477/<name>   (S3/cloud urls are REJECTED in PREPARE)
#   md5 = md5 of the ENTIRE 3mf file (not the plate_1.gcode.md5 inside it)
#   ams_mapping = [tray] with use_ams:true  (else filament-runout fail)
set -euo pipefail

FILE="${1:?usage: print_file.sh <file.gcode.3mf> [ams_tray] [printer_ip] [dongle_ip]}"
TRAY="${2:-1}"
PRINTER="${3:-192.168.1.154}"
DONGLE="${4:-192.168.1.236}"
PORT=8477

[ -f "$FILE" ] || { echo "no such file: $FILE"; exit 1; }
FILE="$(cd "$(dirname "$FILE")" && pwd)/$(basename "$FILE")"
NAME=$(basename "$FILE")
DIR=$(dirname "$FILE")
MYIP=$(ipconfig getifaddr en0 || ipconfig getifaddr en1)

# HTTP server (idempotent)
if ! curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$PORT/$NAME"; then
  echo "[*] starting http.server :$PORT in $DIR"
  (cd "$DIR" && nohup python3 -m http.server $PORT --bind 0.0.0.0 >/tmp/bs-http.log 2>&1 &)
  sleep 1
fi

MD5=$(md5 -q "$FILE")

# state preflight
STATE=$(curl -s --max-time 8 "http://$DONGLE/printer" | python3 -c "import json,sys; print(json.load(sys.stdin).get('print',{}).get('gcode_state','?'))")
echo "[*] printer state: $STATE"

cat > /tmp/bs-printcmd.json <<EOF
{"print":{"sequence_id":$((RANDOM+900000)),"command":"project_file","param":"Metadata/plate_1.gcode","url":"http://$MYIP:$PORT/$NAME","md5":"$MD5","project_id":"0","profile_id":"0","task_id":"0","subtask_id":"0","use_ams":true,"ams_mapping":[$TRAY],"bed_leveling":true,"flow_cali":false,"vibration_cali":true,"layer_inspect":true,"timelapse":false}}
EOF

echo "[*] firing signed project_file via dongle $DONGLE ($NAME, ams tray $TRAY)"
R=$(curl -s --max-time 20 -X POST "http://$DONGLE/cmd" -H "Content-Type: application/json" --data @/tmp/bs-printcmd.json)
echo "[*] ack: $R"

echo "[*] watching state (printer fetches file from this Mac over LAN)..."
for i in $(seq 1 12); do
  sleep 15
  S=$(curl -s --max-time 8 "http://$DONGLE/printer" | python3 -c "
import json,sys
try:
    p=json.load(sys.stdin).get('print',{})
    print(p.get('gcode_state'),'|',p.get('mc_percent'),'|',p.get('nozzle_temper'),'/',p.get('nozzle_target_temper'))
except: print('parse-fail')")
  echo "t+$((i*15))s: $S"
  case "$S" in RUNNING*) echo "=== RUNNING ==="; exit 0;; esac
done
echo "=== did not reach RUNNING in 3 min — check dongle /printer and /logs ==="
exit 1
