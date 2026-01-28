#!/bin/bash

set -e

# Create build directory if it doesn't exist
mkdir -p build
cd build

# Configure and build with WS2812 RGB LED support (RP2040-Zero)
cmake -DUSE_WS2812=ON ..
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

echo ""
echo "Build complete! Firmware: build/hecate.uf2"
