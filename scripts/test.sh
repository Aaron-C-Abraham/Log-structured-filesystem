#!/bin/bash
#
# LSFS Test Script
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

TEST_DIR="/tmp/lsfs_test"
DISK_IMAGE="$TEST_DIR/disk.img"
MOUNT_POINT="$TEST_DIR/mnt"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

PASSED=0
FAILED=0

# Print test result
pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
    ((PASSED++))
}

fail() {
    echo -e "${RED}[FAIL]${NC} $1"
    ((FAILED++))
}

info() {
    echo -e "${YELLOW}[INFO]${NC} $1"
}

# Cleanup function
cleanup() {
    info "Cleaning up..."
    if mountpoint -q "$MOUNT_POINT" 2>/dev/null; then
        fusermount -u "$MOUNT_POINT" 2>/dev/null || true
        sleep 1
    fi
    rm -rf "$TEST_DIR"
}

# Set up trap for cleanup
trap cleanup EXIT

# Check if build exists
if [ ! -x "$BUILD_DIR/lsfs" ]; then
    echo "Error: LSFS not built. Run scripts/build.sh first."
    exit 1
fi

echo "========================================"
echo "LSFS Test Suite"
echo "========================================"
echo ""

# Setup
info "Setting up test environment..."
rm -rf "$TEST_DIR"
mkdir -p "$TEST_DIR"
mkdir -p "$MOUNT_POINT"

# Test 1: Create filesystem
info "Test 1: Create filesystem"
if "$BUILD_DIR/mkfs.lsfs" -s 64 "$DISK_IMAGE" > /dev/null 2>&1; then
    pass "mkfs.lsfs created filesystem"
else
    fail "mkfs.lsfs failed"
    exit 1
fi

# Test 2: Check filesystem
info "Test 2: Check filesystem"
if "$BUILD_DIR/fsck.lsfs" "$DISK_IMAGE" > /dev/null 2>&1; then
    pass "fsck.lsfs passed"
else
    fail "fsck.lsfs failed"
fi

# Test 3: Mount filesystem
info "Test 3: Mount filesystem"
"$BUILD_DIR/lsfs" -f "$DISK_IMAGE" "$MOUNT_POINT" &
LSFS_PID=$!
sleep 2

if mountpoint -q "$MOUNT_POINT"; then
    pass "Filesystem mounted"
else
    fail "Filesystem failed to mount"
    kill $LSFS_PID 2>/dev/null || true
    exit 1
fi

# Test 4: Create file
info "Test 4: Create file"
if echo "Hello, LSFS!" > "$MOUNT_POINT/test.txt"; then
    pass "Created file"
else
    fail "Failed to create file"
fi

# Test 5: Read file
info "Test 5: Read file"
CONTENT=$(cat "$MOUNT_POINT/test.txt")
if [ "$CONTENT" = "Hello, LSFS!" ]; then
    pass "Read file correctly"
else
    fail "File content mismatch: $CONTENT"
fi

# Test 6: Create directory
info "Test 6: Create directory"
if mkdir "$MOUNT_POINT/testdir"; then
    pass "Created directory"
else
    fail "Failed to create directory"
fi

# Test 7: Create file in directory
info "Test 7: Create file in directory"
if echo "Nested file" > "$MOUNT_POINT/testdir/nested.txt"; then
    pass "Created nested file"
else
    fail "Failed to create nested file"
fi

# Test 8: List directory
info "Test 8: List directory"
if ls "$MOUNT_POINT" | grep -q "test.txt"; then
    pass "Listed directory correctly"
else
    fail "Failed to list directory"
fi

# Test 9: Remove file
info "Test 9: Remove file"
if rm "$MOUNT_POINT/test.txt"; then
    pass "Removed file"
else
    fail "Failed to remove file"
fi

# Test 10: Verify removal
info "Test 10: Verify removal"
if [ ! -f "$MOUNT_POINT/test.txt" ]; then
    pass "File removed correctly"
else
    fail "File still exists"
fi

# Test 11: Remove directory
info "Test 11: Remove directory"
if rm -r "$MOUNT_POINT/testdir"; then
    pass "Removed directory"
else
    fail "Failed to remove directory"
fi

# Test 12: Large file
info "Test 12: Large file (1MB)"
if dd if=/dev/zero of="$MOUNT_POINT/large.bin" bs=1024 count=1024 2>/dev/null; then
    SIZE=$(stat -c%s "$MOUNT_POINT/large.bin")
    if [ "$SIZE" = "1048576" ]; then
        pass "Created 1MB file"
    else
        fail "Wrong file size: $SIZE"
    fi
else
    fail "Failed to create large file"
fi

# Test 13: Multiple files
info "Test 13: Multiple files"
for i in $(seq 1 10); do
    echo "File $i" > "$MOUNT_POINT/file$i.txt"
done
COUNT=$(ls "$MOUNT_POINT" | wc -l)
if [ "$COUNT" -ge 10 ]; then
    pass "Created multiple files"
else
    fail "Wrong file count: $COUNT"
fi

# Test 14: Rename file
info "Test 14: Rename file"
if mv "$MOUNT_POINT/file1.txt" "$MOUNT_POINT/renamed.txt"; then
    if [ -f "$MOUNT_POINT/renamed.txt" ] && [ ! -f "$MOUNT_POINT/file1.txt" ]; then
        pass "Renamed file"
    else
        fail "Rename verification failed"
    fi
else
    fail "Failed to rename file"
fi

# Test 15: File permissions
info "Test 15: File permissions"
if chmod 600 "$MOUNT_POINT/renamed.txt"; then
    PERMS=$(stat -c%a "$MOUNT_POINT/renamed.txt")
    if [ "$PERMS" = "600" ]; then
        pass "Set file permissions"
    else
        fail "Wrong permissions: $PERMS"
    fi
else
    fail "Failed to set permissions"
fi

# Test 16: Sync/fsync
info "Test 16: Sync"
if sync; then
    pass "Sync completed"
else
    fail "Sync failed"
fi

# Unmount
info "Unmounting filesystem..."
fusermount -u "$MOUNT_POINT"
wait $LSFS_PID 2>/dev/null || true
sleep 1

# Test 17: Remount and verify
info "Test 17: Remount and verify"
"$BUILD_DIR/lsfs" -f "$DISK_IMAGE" "$MOUNT_POINT" &
LSFS_PID=$!
sleep 2

if mountpoint -q "$MOUNT_POINT"; then
    if [ -f "$MOUNT_POINT/renamed.txt" ]; then
        pass "Data persisted after remount"
    else
        fail "Data lost after remount"
    fi
else
    fail "Failed to remount"
fi

# Final unmount
fusermount -u "$MOUNT_POINT"
wait $LSFS_PID 2>/dev/null || true

# Test 18: Final fsck
info "Test 18: Final filesystem check"
if "$BUILD_DIR/fsck.lsfs" "$DISK_IMAGE" > /dev/null 2>&1; then
    pass "Final fsck passed"
else
    fail "Final fsck failed"
fi

echo ""
echo "========================================"
echo "Test Results"
echo "========================================"
echo -e "Passed: ${GREEN}$PASSED${NC}"
echo -e "Failed: ${RED}$FAILED${NC}"
echo ""

if [ $FAILED -gt 0 ]; then
    exit 1
fi

exit 0
