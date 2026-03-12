#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 2 ]; then
    echo "Usage: ./build.sh <WIFI_SSID> <WIFI_PASSWORD>"
    echo ""
    echo "Example: ./build.sh MyNetwork MyPassword123"
    exit 1
fi

SSID="$1"
PASS="$2"
IMAGE="vanfan-build"
MAX_SIZE=$((1536 * 1024))  # 1.5MB in bytes

echo "══════════════════════════════════════"
echo "  Building VanFan Controller"
echo "  WiFi SSID: $SSID"
echo "══════════════════════════════════════"
echo ""

docker build \
    --build-arg WIFI_SSID="$SSID" \
    --build-arg WIFI_PASS="$PASS" \
    -t "$IMAGE" .

mkdir -p firmware

CID=$(docker create "$IMAGE")
docker cp "$CID:/project/build/bootloader/bootloader.bin" firmware/
docker cp "$CID:/project/build/partition_table/partition-table.bin" firmware/
docker cp "$CID:/project/build/vanfan.bin" firmware/
docker cp "$CID:/project/build/ota_data_initial.bin" firmware/
docker rm "$CID" > /dev/null

BIN_SIZE=$(stat -f%z firmware/vanfan.bin 2>/dev/null || stat -c%s firmware/vanfan.bin 2>/dev/null)
BIN_KB=$((BIN_SIZE / 1024))

echo ""
echo "══════════════════════════════════════"
echo "  Build complete!"
echo "  Firmware files in ./firmware/"
echo "  Binary size: ${BIN_KB}KB"

if [ "$BIN_SIZE" -gt "$MAX_SIZE" ]; then
    echo ""
    echo "  WARNING: Binary exceeds 1.5MB limit!"
    echo "══════════════════════════════════════"
    exit 1
fi

echo ""
echo "  Flash with:"
echo "    ./flash.sh"
echo "══════════════════════════════════════"
