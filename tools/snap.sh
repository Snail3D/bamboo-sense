#!/bin/bash
# Grab a bed snapshot from a BambooSense dongle. Usage: snap.sh [host] [out.jpg]
HOST=${1:-bamboo-sense.local}
OUT=${2:-/tmp/bamboo-sense.jpg}
curl -s -o "$OUT" "http://$HOST/snapshot" && file "$OUT" && echo "$OUT"
