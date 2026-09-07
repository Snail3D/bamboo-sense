#!/bin/bash
# BambooSense check-in: one call = health + live printer state + bed photo.
# Usage: checkin.sh [dongle-host] [out-dir]
HOST=${1:-192.168.1.236}
DIR=${2:-/tmp/bamboo-checkins}
mkdir -p "$DIR"
TS=$(date +%Y%m%d-%H%M%S)

STATUS=$(curl -s --max-time 6 "http://$HOST/status")
PRINT=$(curl -s --max-time 6 "http://$HOST/printer" | python3 -c "
import json,sys
try:
    d=json.load(sys.stdin); p=d.get('print',{})
    print(json.dumps({'state':p.get('gcode_state'),'spd_lvl':p.get('spd_lvl'),'layer':p.get('layer_num'),'total_layers':p.get('total_layer_num'),'pct':p.get('mc_percent'),'file':p.get('subtask_name'),'remaining_m':p.get('mc_remaining_time')}))
except Exception as e: print('{\"error\":\"report parse\"}')")
SNAP="$DIR/bed-$TS.jpg"
curl -s --max-time 10 -o "$SNAP" "http://$HOST/snapshot"

echo "== dongle =="; echo "$STATUS"
echo "== printer =="; echo "$PRINT"
echo "== bed photo =="; ls -la "$SNAP" 2>/dev/null | awk '{print $9, $5" bytes"}'
