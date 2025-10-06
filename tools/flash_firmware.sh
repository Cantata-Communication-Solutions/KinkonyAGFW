#!/bin/bash
# Flash KinkonyAGFW firmware using esptool.py
# Usage: ./flash_firmware.sh /dev/ttyUSB0

PORT=${1:-/dev/ttyUSB0}
BIN_PATH="bin/Haptique_Kincony_AG_Firmware_V1.bin"

if ! command -v esptool.py &> /dev/null
then
    echo "❌ esptool.py not found. Please install with: pip install esptool"
    exit 1
fi

if [ ! -f "$BIN_PATH" ]; then
    echo "❌ Firmware file not found at $BIN_PATH"
    exit 1
fi

echo "🚀 Flashing firmware to ESP32 at $PORT ..."
esptool.py --chip esp32 --port "$PORT" --baud 921600 write_flash 0x1000 "$BIN_PATH"
if [ $? -eq 0 ]; then
    echo "✅ Firmware flashed successfully!"
else
    echo "❌ Flashing failed."
fi
