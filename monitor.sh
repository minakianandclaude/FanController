#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv"

# Set up venv with pyserial
if [ ! -d "$VENV_DIR" ]; then
    echo "Creating virtual environment..."
    python3 -m venv "$VENV_DIR"
fi
source "$VENV_DIR/bin/activate"

if ! python -c "import serial" 2>/dev/null; then
    echo "Installing pyserial..."
    pip install pyserial
fi

# Auto-detect serial port
find_port() {
    local ports=()

    # macOS USB serial devices
    for p in /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.SLAB_USB*; do
        [ -e "$p" ] && ports+=("$p")
    done

    # Linux USB serial devices
    for p in /dev/ttyACM* /dev/ttyUSB*; do
        [ -e "$p" ] && ports+=("$p")
    done

    if [ ${#ports[@]} -eq 0 ]; then
        echo ""
    elif [ ${#ports[@]} -eq 1 ]; then
        echo "${ports[0]}"
    else
        echo "Multiple serial ports found:" >&2
        for i in "${!ports[@]}"; do
            echo "  [$((i+1))] ${ports[$i]}" >&2
        done
        read -rp "Select port [1]: " choice
        choice="${choice:-1}"
        if [[ "$choice" =~ ^[0-9]+$ ]] && [ "$choice" -ge 1 ] && [ "$choice" -le ${#ports[@]} ]; then
            echo "${ports[$((choice-1))]}"
        else
            echo "Invalid selection." >&2
            echo ""
        fi
    fi
}

PORT="${1:-}"
if [ -z "$PORT" ]; then
    PORT=$(find_port)
    if [ -z "$PORT" ]; then
        echo "Error: No USB serial device found."
        echo ""
        echo "Make sure the ESP32-S3 is plugged in, then either:"
        echo "  - Re-run this script"
        echo "  - Specify the port manually: ./monitor.sh /dev/cu.usbmodemXXXXX"
        echo ""
        echo "List available ports with: ls /dev/cu.usb* /dev/ttyACM* /dev/ttyUSB* 2>/dev/null"
        exit 1
    fi
fi

if [ ! -e "$PORT" ]; then
    echo "Error: Port $PORT does not exist."
    echo ""
    echo "Available ports:"
    ls /dev/cu.usb* /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || echo "  (none found)"
    exit 1
fi

echo "Connecting to $PORT at 115200 baud..."
echo "Press Ctrl+] to exit."
echo ""
python -m serial.tools.miniterm "$PORT" 115200
