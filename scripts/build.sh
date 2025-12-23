#!/bin/bash
#
# LSFS Build Script
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

# Parse arguments
BUILD_TYPE="Release"
CLEAN=0

while [[ $# -gt 0 ]]; do
    case $1 in
        -d|--debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        -c|--clean)
            CLEAN=1
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  -d, --debug    Build with debug symbols"
            echo "  -c, --clean    Clean build directory first"
            echo "  -h, --help     Show this help"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Check dependencies
check_dep() {
    if ! command -v "$1" &> /dev/null; then
        echo "Error: $1 is required but not installed."
        exit 1
    fi
}

check_dep cmake
check_dep pkg-config

# Check for libfuse3
if ! pkg-config --exists fuse3; then
    echo "Error: libfuse3 is required but not installed."
    echo "Install with: sudo apt install libfuse3-dev fuse3"
    exit 1
fi

# Clean if requested
if [ $CLEAN -eq 1 ] && [ -d "$BUILD_DIR" ]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure
echo "Configuring with CMake (${BUILD_TYPE})..."
cmake -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "$PROJECT_DIR"

# Build
echo "Building..."
cmake --build . -- -j$(nproc)

echo ""
echo "Build complete!"
echo ""
echo "Executables:"
echo "  $BUILD_DIR/lsfs         - LSFS FUSE daemon"
echo "  $BUILD_DIR/mkfs.lsfs    - Format utility"
echo "  $BUILD_DIR/fsck.lsfs    - Filesystem check utility"
echo "  $BUILD_DIR/lsfs-debug   - Debug utility"
echo ""
echo "Quick start:"
echo "  1. Create disk image:  ./mkfs.lsfs -s 256 /tmp/disk.img"
echo "  2. Create mount point: mkdir -p /tmp/lsfs"
echo "  3. Mount filesystem:   ./lsfs -f /tmp/disk.img /tmp/lsfs"
echo "  4. Unmount:            fusermount -u /tmp/lsfs"
