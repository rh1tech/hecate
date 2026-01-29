#!/bin/bash
#
# release.sh - Build release version of Hecate
#
# Creates UF2 file for the USB to PS/2 converter.
# Builds with WS2812 RGB LED support (RP2040-Zero).
#
# Output format: hecate_X_YY.uf2
#   X  = Major version
#   YY = Minor version (zero-padded)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Version file
VERSION_FILE="version.txt"

# Read last version or initialize
if [[ -f "$VERSION_FILE" ]]; then
    read -r LAST_MAJOR LAST_MINOR < "$VERSION_FILE"
else
    LAST_MAJOR=1
    LAST_MINOR=0
fi

# Calculate next version (for default suggestion)
NEXT_MINOR=$((LAST_MINOR + 1))
NEXT_MAJOR=$LAST_MAJOR
if [[ $NEXT_MINOR -ge 100 ]]; then
    NEXT_MAJOR=$((NEXT_MAJOR + 1))
    NEXT_MINOR=0
fi

# Interactive version input
echo ""
echo -e "${CYAN}┌─────────────────────────────────────────────────────────────────┐${NC}"
echo -e "${CYAN}│                     Hecate Release Builder                      │${NC}"
echo -e "${CYAN}│              USB to PS/2 Keyboard & Mouse Converter             │${NC}"
echo -e "${CYAN}└─────────────────────────────────────────────────────────────────┘${NC}"
echo ""
echo -e "Last version: ${YELLOW}${LAST_MAJOR}.$(printf '%02d' $LAST_MINOR)${NC}"
echo ""

DEFAULT_VERSION="${NEXT_MAJOR}.$(printf '%02d' $NEXT_MINOR)"
read -p "Enter version [default: $DEFAULT_VERSION]: " INPUT_VERSION
INPUT_VERSION=${INPUT_VERSION:-$DEFAULT_VERSION}

# Parse version (handle both "1.00" and "1 00" formats)
if [[ "$INPUT_VERSION" == *"."* ]]; then
    MAJOR="${INPUT_VERSION%%.*}"
    MINOR="${INPUT_VERSION##*.}"
else
    read -r MAJOR MINOR <<< "$INPUT_VERSION"
fi

# Remove leading zeros for arithmetic, then re-pad
MINOR=$((10#$MINOR))
MAJOR=$((10#$MAJOR))

# Validate
if [[ $MAJOR -lt 1 ]]; then
    echo -e "${RED}Error: Major version must be >= 1${NC}"
    exit 1
fi
if [[ $MINOR -lt 0 || $MINOR -ge 100 ]]; then
    echo -e "${RED}Error: Minor version must be 0-99${NC}"
    exit 1
fi

# Format version string
VERSION="${MAJOR}_$(printf '%02d' $MINOR)"
echo ""
echo -e "${GREEN}Building release version: ${MAJOR}.$(printf '%02d' $MINOR)${NC}"

# Save new version
echo "$MAJOR $MINOR" > "$VERSION_FILE"

# Create release directory
RELEASE_DIR="$SCRIPT_DIR/releases"
mkdir -p "$RELEASE_DIR"

# Output filename
OUTPUT_NAME="hecate_${VERSION}.uf2"

echo ""
echo -e "${YELLOW}Building firmware...${NC}"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo -e "${CYAN}Building: $OUTPUT_NAME${NC}"
echo -e "  Features: WS2812 RGB LED, Hybrid USB (Type-C + PIO-USB)"

# Clean and create build directory
rm -rf build
mkdir build
cd build

# Configure with CMake (WS2812 enabled for RP2040-Zero)
echo -e "  Configuring..."
cmake .. -DUSE_WS2812=ON > /dev/null 2>&1

# Build
echo -e "  Compiling..."
if make -j8 > /dev/null 2>&1; then
    # Copy UF2 to release directory
    if [[ -f "hecate.uf2" ]]; then
        cp "hecate.uf2" "$RELEASE_DIR/$OUTPUT_NAME"
        echo -e "  ${GREEN}✓ Success${NC} → releases/$OUTPUT_NAME"
    else
        echo -e "  ${RED}✗ UF2 not found${NC}"
        exit 1
    fi
else
    echo -e "  ${RED}✗ Build failed${NC}"
    cd "$SCRIPT_DIR"
    exit 1
fi

cd "$SCRIPT_DIR"

# Clean up build directory
rm -rf build

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo -e "${GREEN}Release build complete!${NC}"
echo ""
echo "Release file: $RELEASE_DIR/$OUTPUT_NAME"
echo ""
ls -la "$RELEASE_DIR/$OUTPUT_NAME" 2>/dev/null | awk '{print "  " $9 " (" $5 " bytes)"}'
echo ""
echo -e "Version: ${CYAN}${MAJOR}.$(printf '%02d' $MINOR)${NC}"
echo ""
