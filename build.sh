#!/usr/bin/env bash
set -euo pipefail

if [ $# -ge 2 ]; then
    SSID="$1"
    PASS="$2"
elif [ -f .env ]; then
    # shellcheck source=/dev/null
    source .env
    SSID="${WIFI_SSID:-}"
    PASS="${WIFI_PASSWORD:-}"
else
    echo "Error: No .env file found and no arguments provided."
    echo ""
    echo "Create a .env file with:"
    echo "  WIFI_SSID='YourSSID'"
    echo "  WIFI_PASSWORD='YourPassword'"
    echo ""
    echo "Or pass as arguments: ./build.sh <SSID> <PASSWORD>"
    exit 1
fi

if [ -z "$SSID" ] || [ -z "$PASS" ]; then
    echo "Error: WIFI_SSID and WIFI_PASSWORD must be set"
    echo "Provide as arguments or in .env file"
    exit 1
fi
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
docker cp "$CID:/project/build/vanfan.elf" firmware/ 2>/dev/null || true
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
