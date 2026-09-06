#!/bin/bash
# Flash BambooSense over WiFi (OTA). Usage: ota_push.sh [host] [bin]
HOST=${1:-bamboo-sense.local}
BIN=${2:-firmware/.pio/build/printer/firmware.bin}
echo "OTA -> http://$HOST/ota  ($BIN)"
curl -s -F "update=@$BIN" "http://$HOST/ota"; echo
