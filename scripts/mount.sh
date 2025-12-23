#!/bin/bash
#
# LSFS Mount Helper Script
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

# Default values
DISK_IMAGE=""
MOUNT_POINT=""
SIZE_MB=256
FOREGROUND=0
DEBUG=0
FORMAT=0

# Print usage
usage() {
    echo "Usage: $0 [options] <disk_image> <mount_point>"
    echo ""
    echo "Options:"
    echo "  -s, --size <MB>      Size for new filesystem (default: 256)"
    echo "  -f, --foreground     Run in foreground"
    echo "  -d, --debug          Enable debug output (implies -f)"
    echo "  -F, --format         Format disk image before mounting"
    echo "  -h, --help           Show this help"
    echo ""
    echo "Examples:"
    echo "  $0 -F /tmp/disk.img /tmp/lsfs    # Create and mount new filesystem"
    echo "  $0 /tmp/disk.img /tmp/lsfs       # Mount existing filesystem"
    echo "  $0 -f /tmp/disk.img /tmp/lsfs    # Mount in foreground"
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -s|--size)
            SIZE_MB="$2"
            shift 2
            ;;
        -f|--foreground)
            FOREGROUND=1
            shift
            ;;
        -d|--debug)
            DEBUG=1
            FOREGROUND=1
            shift
            ;;
        -F|--format)
            FORMAT=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo "Unknown option: $1"
            usage
            exit 1
            ;;
        *)
            if [ -z "$DISK_IMAGE" ]; then
                DISK_IMAGE="$1"
            elif [ -z "$MOUNT_POINT" ]; then
                MOUNT_POINT="$1"
            else
                echo "Too many arguments"
                usage
                exit 1
            fi
            shift
            ;;
    esac
done

# Check arguments
if [ -z "$DISK_IMAGE" ] || [ -z "$MOUNT_POINT" ]; then
    usage
    exit 1
fi

# Check if build exists
if [ ! -x "$BUILD_DIR/lsfs" ]; then
    echo "Error: LSFS not built. Run scripts/build.sh first."
    exit 1
fi

# Format if requested or if disk doesn't exist
if [ $FORMAT -eq 1 ] || [ ! -f "$DISK_IMAGE" ]; then
    echo "Creating filesystem on $DISK_IMAGE ($SIZE_MB MB)..."
    "$BUILD_DIR/mkfs.lsfs" -s "$SIZE_MB" "$DISK_IMAGE"
fi

# Create mount point if needed
if [ ! -d "$MOUNT_POINT" ]; then
    echo "Creating mount point $MOUNT_POINT..."
    mkdir -p "$MOUNT_POINT"
fi

# Build mount command
CMD="$BUILD_DIR/lsfs"
if [ $FOREGROUND -eq 1 ]; then
    CMD="$CMD -f"
fi
if [ $DEBUG -eq 1 ]; then
    CMD="$CMD -d"
fi
CMD="$CMD $DISK_IMAGE $MOUNT_POINT"

echo "Mounting LSFS..."
echo "  Disk image: $DISK_IMAGE"
echo "  Mount point: $MOUNT_POINT"
echo ""

if [ $FOREGROUND -eq 1 ]; then
    echo "Running in foreground. Press Ctrl+C to unmount."
    echo ""
fi

exec $CMD
