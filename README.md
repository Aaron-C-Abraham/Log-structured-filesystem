# LSFS - Log-Structured Filesystem

[![Build Status](https://github.com/yourusername/lsfs/workflows/CI/badge.svg)](https://github.com/yourusername/lsfs/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

A FUSE-based log-structured filesystem implementation in C, demonstrating modern storage principles and crash recovery mechanisms.

## Overview

LSFS (Log-Structured FileSystem) is a userspace filesystem that treats the disk as an append-only log. Unlike traditional filesystems that update data in place, LSFS appends all modifications sequentially, converting random writes to sequential writes and enabling simple crash recovery through log replay.

### Key Features

- **Append-Only Writes**: All data and metadata written sequentially for optimal write performance
- **Copy-on-Write**: Updates create new versions, never modify existing data
- **Crash Recovery**: Checkpoint-based recovery with log replay
- **Garbage Collection**: Background segment cleaning with cost-benefit policy
- **POSIX Compliant**: Standard filesystem operations (create, read, write, delete, rename, etc.)
- **FUSE 3.x**: Uses the low-level FUSE API for maximum control

### Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                      USER APPLICATIONS                           │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                    LINUX KERNEL (VFS + FUSE)                     │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                       LSFS FUSE DAEMON                           │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐  │
│  │ FUSE Ops    │  │ Inode Cache │  │ Directory Operations    │  │
│  └─────────────┘  └─────────────┘  └─────────────────────────┘  │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐  │
│  │ Segment Mgr │  │ Inode Map   │  │ Checkpoint Manager      │  │
│  └─────────────┘  └─────────────┘  └─────────────────────────┘  │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐  │
│  │ Block I/O   │  │ Buffer Pool │  │ Garbage Collector       │  │
│  └─────────────┘  └─────────────┘  └─────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
                       ┌───────────────┐
                       │  Disk Image   │
                       └───────────────┘
```

## Requirements

### System Requirements

- Linux (Ubuntu 22.04+ or Fedora 38+ recommended)
- FUSE 3.10+
- GCC 11+ or Clang 14+ with C11 support
- CMake 3.16+

### Dependencies

**Ubuntu/Debian:**
```bash
sudo apt update
sudo apt install build-essential cmake pkg-config libfuse3-dev fuse3
```

**Fedora:**
```bash
sudo dnf install gcc cmake pkg-config fuse3-devel fuse3
```

**Arch Linux:**
```bash
sudo pacman -S base-devel cmake pkgconf fuse3
```

## Building

### Quick Build

```bash
git clone https://github.com/yourusername/lsfs.git
cd lsfs
./scripts/build.sh
```

### Manual Build

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

### Build Options

```bash
# Debug build with symbols
./scripts/build.sh --debug

# Clean build
./scripts/build.sh --clean
```

## Usage

### Creating a Filesystem

```bash
# Create a 256 MB filesystem
./build/mkfs.lsfs -s 256 /path/to/disk.img

# Create a 1 GB filesystem
./build/mkfs.lsfs -s 1024 /path/to/disk.img
```

### Mounting

```bash
# Create mount point
mkdir -p /mnt/lsfs

# Mount in foreground (useful for debugging)
./build/lsfs -f /path/to/disk.img /mnt/lsfs

# Mount in background
./build/lsfs /path/to/disk.img /mnt/lsfs

# Mount with debug output
./build/lsfs -d /path/to/disk.img /mnt/lsfs
```

### Using the Filesystem

```bash
# Create files and directories
echo "Hello, LSFS!" > /mnt/lsfs/hello.txt
mkdir /mnt/lsfs/mydir
cp /etc/passwd /mnt/lsfs/mydir/

# Read files
cat /mnt/lsfs/hello.txt

# List contents
ls -la /mnt/lsfs/
```

### Unmounting

```bash
# Unmount the filesystem
fusermount -u /mnt/lsfs

# Force unmount if busy
fusermount -uz /mnt/lsfs
```

### Filesystem Check

```bash
# Check filesystem integrity
./build/fsck.lsfs /path/to/disk.img

# Check with repair
./build/fsck.lsfs -r /path/to/disk.img

# Verbose output
./build/fsck.lsfs -v /path/to/disk.img
```

### Debug Utility

```bash
# Dump superblock
./build/lsfs-debug /path/to/disk.img superblock

# Dump checkpoints
./build/lsfs-debug /path/to/disk.img checkpoint

# Dump inode map
./build/lsfs-debug /path/to/disk.img imap

# Dump segment
./build/lsfs-debug /path/to/disk.img segment 0

# Dump all structures
./build/lsfs-debug /path/to/disk.img all
```

## Testing

```bash
# Run the test suite
./scripts/test.sh
```

The test suite covers:
- Filesystem creation and mounting
- File and directory operations
- Large file handling
- Persistence across remounts
- Filesystem integrity checks

## On-Disk Format

### Disk Layout

| Region | Blocks | Description |
|--------|--------|-------------|
| Superblock | 0 | Filesystem metadata |
| Checkpoint 0 | 1-256 | First checkpoint region |
| Checkpoint 1 | 257-512 | Second checkpoint region |
| Segment Table | 513-1024 | Segment usage tracking |
| Log Segments | 1025+ | Data and metadata segments |

### Key Parameters

| Parameter | Value |
|-----------|-------|
| Block Size | 4 KB |
| Segment Size | 4 MB (1024 blocks) |
| Max Filesystem Size | 1 GB |
| Max File Size | ~4 GB |
| Max Files | 65,536 |
| Max Filename Length | 255 bytes |

## How It Works

### Write Path

1. Application issues write request
2. Data buffered in current segment
3. When segment is full, flush to disk
4. Update inode map with new block locations
5. Periodically write checkpoints

### Read Path

1. Look up inode in inode map
2. Find block location from inode
3. Read block from disk (through buffer cache)
4. Return data to application

### Crash Recovery

1. Read superblock and find active checkpoint
2. Load inode map from checkpoint
3. Scan log from checkpoint position
4. Replay any segments written after checkpoint
5. Write new checkpoint

### Garbage Collection

1. Monitor free segment count
2. When low, select segment with lowest utilization
3. Copy live blocks to new segment
4. Update inode map
5. Free cleaned segment

## Project Structure

```
lsfs/
├── CMakeLists.txt          # Build configuration
├── README.md               # This file
├── LICENSE                 # MIT License
├── include/
│   ├── lsfs.h              # Main header file
│   └── ondisk.h            # On-disk format definitions
├── src/
│   ├── main.c              # FUSE daemon entry point
│   ├── fuse_ops.c          # FUSE operation handlers
│   ├── io.c                # Block I/O layer
│   ├── inode.c             # Inode operations
│   ├── directory.c         # Directory operations
│   ├── segment.c           # Segment management
│   ├── imap.c              # Inode map
│   ├── checkpoint.c        # Checkpoint system
│   └── gc.c                # Garbage collector
├── tools/
│   ├── mkfs.lsfs.c         # Filesystem formatter
│   ├── fsck.lsfs.c         # Filesystem checker
│   └── lsfs-debug.c        # Debug utility
├── scripts/
│   ├── build.sh            # Build script
│   ├── mount.sh            # Mount helper
│   └── test.sh             # Test suite
└── tests/
    └── CMakeLists.txt      # Test configuration
```

## Contributing

Contributions are welcome! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

### Development Setup

```bash
# Clone the repository
git clone https://github.com/yourusername/lsfs.git
cd lsfs

# Build in debug mode
./scripts/build.sh --debug

# Run tests
./scripts/test.sh
```

## Known Limitations

- Maximum filesystem size is 1 GB
- No extended attributes support
- No hard link support (yet)
- Single-threaded FUSE operations
- No quota support

## References

- [The Design and Implementation of a Log-Structured File System](https://people.eecs.berkeley.edu/~brewer/cs262/LFS.pdf) - Rosenblum & Ousterhout (1991)
- [FUSE Documentation](https://libfuse.github.io/doxygen/)
- [Operating Systems: Three Easy Pieces](https://pages.cs.wisc.edu/~remzi/OSTEP/) - Chapters 39-42

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Author

Aaron C. Abraham - [@Aaron-C-Abraham](https://github.com/Aaron-C-Abraham)

## Acknowledgments

- The FUSE development team
- The original LFS paper authors
- The Linux kernel filesystem developers
