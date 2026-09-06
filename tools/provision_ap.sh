#!/bin/bash
# First-boot provisioning: join the dongle's AP from this Mac (works even while
# the Mac is on ethernet) and hand it the home WiFi creds.
# Usage: provision_ap.sh <dongle-ap-ssid> <home-ssid> <home-pass> [name]
set -e
AP=$1; SSID=$2; PASS=$3; NAME=${4:-bamboo-sense}
IFACE=${IFACE:-en0}

echo ">>> joining dongle AP '$AP' on $IFACE..."
networksetup -setairportnetwork "$IFACE" "$AP" "$PASS_DUMMY" 2>/dev/null || true
networksetup -setairportnetwork "$IFACE" "$AP" "" 2>/dev/null || true   # open AP
sleep 3
IP=""
for i in $(seq 1 15); do
  IP=$(ipconfig getifaddr $IFACE 2>/dev/null | grep '^192\.168\.4\.') && break
  sleep 1
done
[ -z "$IP" ] && { echo "!! could not get 192.168.4.x address — did we join the AP?"; exit 1; }
echo ">>> got $IP, sending config..."
curl -s -X POST "http://192.168.4.1/config" \
  --data-urlencode "ssid=$SSID" \
  --data-urlencode "pass=$PASS" \
  --data-urlencode "name=$NAME"
echo
echo ">>> dongle is rebooting onto '$SSID'. Leaving the AP..."
networksetup -removepreferredwirelessnetwork "$IFACE" "$AP" 2>/dev/null || true
sleep 8
echo ">>> try: curl http://$NAME.local/status"
