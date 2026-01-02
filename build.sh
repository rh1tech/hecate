#!/bin/bash

set -e

# Create build directory if it doesn't exist
mkdir -p build
cd build

# Configure and build
cmake ..
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

echo ""
echo "Build complete! Firmware: build/hecate.uf2"
