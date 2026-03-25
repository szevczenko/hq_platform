#!/bin/bash
#
# ESP-IDF Docker Build Script
#
# Builds the project for ESP32 using the official espressif/idf Docker image (v6.0).
# 
# Requirements:
#   - Docker installed and running
#   - Docker daemon has access to build the project
#
# Usage:
#   ./scripts/esp_build.sh [clean]
#
# Options:
#   clean   - Remove build directory before building
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build_esp"
IDF_VERSION="v6.0"
IDF_TARGET="esp32"

# Check if Docker is installed
if ! command -v docker &> /dev/null; then
    echo "Error: Docker is not installed. Please install Docker to use this script."
    exit 1
fi

# Check if Docker daemon is running
if ! docker info > /dev/null 2>&1; then
    echo "Error: Docker daemon is not running. Please start Docker and try again."
    exit 1
fi

echo "=========================================="
echo "ESP-IDF Docker Build Script"
echo "=========================================="
echo "Project directory: $PROJECT_DIR"
echo "Build directory: $BUILD_DIR"
echo "IDF Target: $IDF_TARGET"
echo "IDF Version: $IDF_VERSION"
echo ""

# Handle clean argument
if [ "$1" = "clean" ]; then
    echo "Removing existing build directory..."
    rm -rf "$BUILD_DIR"
fi

# Create build directory if it doesn't exist
mkdir -p "$BUILD_DIR"

echo "Building with Docker image: espressif/idf:$IDF_VERSION"
echo ""

# Run Docker build
docker run --rm \
    -v "$PROJECT_DIR":/project \
    -w /project \
    -u "$UID" \
    -e HOME=/tmp \
    -e IDF_GIT_SAFE_DIR=/project:/opt/esp/idf/components/openthread/openthread \
    -e IDF_TARGET=$IDF_TARGET \
    "espressif/idf:$IDF_VERSION" \
    idf.py \
        -B build_esp \
        build

echo ""
echo "=========================================="
echo "Build completed successfully!"
echo "=========================================="
echo "Build artifacts: $BUILD_DIR"
echo ""
echo "To flash the build (requires board connected):"
echo "  docker run --rm -v $PROJECT_DIR:/project -w /project -u $UID -e HOME=/tmp --device /dev/ttyUSB0 espressif/idf:$IDF_VERSION idf.py -B build_esp flash"
echo ""
