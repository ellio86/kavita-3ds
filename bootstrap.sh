#!/bin/bash
# bootstrap.sh - Download vendored single-file libraries
# Run this once before building: bash bootstrap.sh

set -e

echo "Downloading vendored libraries..."

# cJSON (MIT license) - lightweight JSON parser
CJSON_VERSION="1.7.18"
CJSON_BASE="https://raw.githubusercontent.com/DaveGamble/cJSON/v${CJSON_VERSION}"

echo "  -> cJSON ${CJSON_VERSION}"
curl -fsSL "${CJSON_BASE}/cJSON.h" -o libs/cJSON.h
curl -fsSL "${CJSON_BASE}/cJSON.c" -o libs/cJSON.c

# stb_image (public domain) - JPEG/PNG decoder
STB_COMMIT="master"
STB_BASE="https://raw.githubusercontent.com/nothings/stb/${STB_COMMIT}"

echo "  -> stb_image"
curl -fsSL "${STB_BASE}/stb_image.h" -o libs/stb_image.h

echo ""
echo "Done! You can now build with: make"
echo ""
echo "Build requirements:"
echo "  - devkitPro installed with 3ds-dev, 3ds-citro2d, 3ds-citro3d packages"
echo "  - DEVKITPRO and DEVKITARM environment variables set"
echo "  - Run from a devkitPro-enabled shell (MSYS2 on Windows)"
