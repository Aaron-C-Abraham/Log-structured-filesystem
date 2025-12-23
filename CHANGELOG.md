# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Initial implementation of LSFS
- FUSE 3.x low-level API integration
- Log-structured storage with append-only writes
- Checkpoint-based crash recovery
- Background garbage collection
- Basic POSIX operations:
  - File operations: create, open, read, write, unlink
  - Directory operations: mkdir, rmdir, readdir
  - Metadata operations: getattr, setattr, rename
  - Sync operations: fsync, statfs
- mkfs.lsfs utility for filesystem creation
- fsck.lsfs utility for filesystem checking
- lsfs-debug utility for debugging and inspection
- Comprehensive test suite
- GitHub Actions CI pipeline

### Technical Details
- Block size: 4 KB
- Segment size: 4 MB (1024 blocks)
- Maximum filesystem size: 1 GB
- Maximum file size: ~4 GB (with indirect blocks)
- Maximum files: 65,536

## [0.1.0] - 2025-XX-XX

### Added
- Initial public release

---

## Version History

### Planned Features

- [ ] Extended attributes (xattr) support
- [ ] Hard link support
- [ ] Symbolic link improvements
- [ ] Multi-threaded FUSE operations
- [ ] Online filesystem resizing
- [ ] Compression support
- [ ] Encryption support

### Known Issues

- Filesystem size limited to 1 GB
- No support for special files (devices, sockets)
- Single-threaded operation may limit performance

---

[Unreleased]: https://github.com/yourusername/lsfs/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/yourusername/lsfs/releases/tag/v0.1.0
