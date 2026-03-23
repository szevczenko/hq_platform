#!/bin/bash
#
# POSIX Build Script
#
# Creates build_posix directory and compiles the project with default POSIX defconfig.
# Enables testing by default.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build_posix"

echo "=========================================="
echo "POSIX Build Script"
echo "=========================================="
echo "Project directory: $PROJECT_DIR"
echo "Build directory: $BUILD_DIR"
echo ""

# Remove old build directory if it exists
if [ -d "$BUILD_DIR" ]; then
    echo "Removing existing build directory..."
    rm -rf "$BUILD_DIR"
fi

# Create build directory
echo "Creating build directory..."
mkdir -p "$BUILD_DIR"

# Configure CMake
echo "Configuring CMake..."
cd "$BUILD_DIR"
cmake \
    -DCMAKE_TOOLCHAIN_FILE="$PROJECT_DIR/cmake/posix.cmake" \
    -DCONFIG_HQ_PLATFORM_POSIX=y \
    -DCONFIG_OSAL_LOG_LEVEL=3 \
    -DHQ_BUILD_TESTS=ON \
    "$PROJECT_DIR"

# Build project
echo ""
echo "Building project..."
cmake --build . --parallel "$(nproc)"

echo ""
echo "=========================================="
echo "Build completed successfully!"
echo "=========================================="
echo "Test executable: $BUILD_DIR/tests/osal_tests"
echo ""
echo "To run tests:"
echo "  $BUILD_DIR/tests/osal_tests"
echo ""
