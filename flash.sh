#!/bin/bash

set -e

FIRMWARE="build/hecate.uf2"

# Check if firmware exists
if [ ! -f "$FIRMWARE" ]; then
    echo "Error: Firmware not found at $FIRMWARE"
    echo "Run ./build.sh first"
    exit 1
fi

# Flash using picotool
echo "Flashing $FIRMWARE..."
picotool load "$FIRMWARE" -f

echo "Flash complete!"
