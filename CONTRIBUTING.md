# Contributing to LSFS

Thank you for your interest in contributing to LSFS! This document provides guidelines and information for contributors.

## Table of Contents

- [Code of Conduct](#code-of-conduct)
- [Getting Started](#getting-started)
- [How to Contribute](#how-to-contribute)
- [Development Setup](#development-setup)
- [Coding Standards](#coding-standards)
- [Commit Guidelines](#commit-guidelines)
- [Pull Request Process](#pull-request-process)
- [Reporting Bugs](#reporting-bugs)
- [Feature Requests](#feature-requests)

## Code of Conduct

This project follows a simple code of conduct:

- Be respectful and inclusive
- Focus on constructive feedback
- Help others learn and grow
- Accept responsibility for mistakes

## Getting Started

1. Fork the repository on GitHub
2. Clone your fork locally
3. Set up the development environment
4. Create a branch for your changes
5. Make your changes
6. Test your changes
7. Submit a pull request

## How to Contribute

### Types of Contributions

- **Bug fixes**: Fix issues reported in the issue tracker
- **Features**: Implement new functionality
- **Documentation**: Improve or add documentation
- **Tests**: Add or improve test coverage
- **Performance**: Optimize existing code
- **Refactoring**: Improve code structure without changing behavior

### Good First Issues

Look for issues labeled `good first issue` or `help wanted` in the issue tracker. These are great starting points for new contributors.

## Development Setup

### Prerequisites

```bash
# Ubuntu/Debian
sudo apt install build-essential cmake pkg-config libfuse3-dev fuse3 git

# Install additional tools for development
sudo apt install valgrind gdb clang-format
```

### Building for Development

```bash
# Clone your fork
git clone https://github.com/YOUR_USERNAME/lsfs.git
cd lsfs

# Add upstream remote
git remote add upstream https://github.com/ORIGINAL_OWNER/lsfs.git

# Build in debug mode
./scripts/build.sh --debug

# Run tests
./scripts/test.sh
```

### Running with Debug Output

```bash
# Create test filesystem
./build/mkfs.lsfs -s 64 /tmp/test.img

# Mount with debug output
./build/lsfs -d /tmp/test.img /tmp/lsfs
```

### Memory Checking with Valgrind

```bash
# Run with valgrind (note: FUSE makes this tricky)
valgrind --leak-check=full ./build/lsfs -f /tmp/test.img /tmp/lsfs
```

## Coding Standards

### C Style Guide

- Use C11 standard
- 4 spaces for indentation (no tabs)
- Maximum line length: 100 characters
- Opening braces on the same line for functions and control structures
- Always use braces for control structures, even single-line bodies

### Example

```c
/*
 * Function description
 */
int example_function(int param1, const char *param2)
{
    if (param1 < 0) {
        return -1;
    }

    for (int i = 0; i < param1; i++) {
        do_something(i);
    }

    return 0;
}
```

### Naming Conventions

- Functions: `lsfs_module_action()` (e.g., `lsfs_inode_write()`)
- Types: `struct lsfs_type_name` (e.g., `struct lsfs_inode`)
- Constants: `LSFS_CONSTANT_NAME` (e.g., `LSFS_BLOCK_SIZE`)
- Local variables: `snake_case`
- Global variables: `g_variable_name`

### Comments

- Use `/* */` for multi-line comments
- Use `//` for single-line comments (C99+)
- Document all public functions with a comment block
- Explain "why" not "what" in inline comments

### Error Handling

- Always check return values
- Use the defined error codes in `lsfs.h`
- Log errors using `LSFS_ERROR()` macro
- Clean up resources on error paths

## Commit Guidelines

### Commit Message Format

```
<type>: <subject>

<body>

<footer>
```

### Types

- `feat`: New feature
- `fix`: Bug fix
- `docs`: Documentation only
- `style`: Formatting, no code change
- `refactor`: Code change that neither fixes a bug nor adds a feature
- `perf`: Performance improvement
- `test`: Adding or updating tests
- `chore`: Build process or auxiliary tool changes

### Example

```
feat: add symbolic link support

Implement symlink and readlink operations for LSFS.
Symbolic links shorter than 64 bytes are stored inline
in the inode structure.

Closes #42
```

### Guidelines

- Use present tense ("add feature" not "added feature")
- Use imperative mood ("move cursor to..." not "moves cursor to...")
- Keep subject line under 50 characters
- Wrap body at 72 characters
- Reference issues in footer

## Pull Request Process

### Before Submitting

1. Ensure your code compiles without warnings
2. Run the test suite and ensure all tests pass
3. Add tests for new functionality
4. Update documentation if needed
5. Rebase on the latest upstream main

### Submitting

1. Push your branch to your fork
2. Open a pull request against the main repository
3. Fill out the pull request template
4. Wait for review

### During Review

- Respond to feedback promptly
- Make requested changes in new commits
- Squash commits before merge if requested
- Be patient - reviews take time

### After Merge

- Delete your branch
- Update your local main branch
- Celebrate!

## Reporting Bugs

### Before Reporting

- Check existing issues to avoid duplicates
- Try to reproduce on the latest version
- Gather relevant information

### Bug Report Template

```markdown
**Description**
A clear description of the bug.

**To Reproduce**
Steps to reproduce the behavior:
1. Create filesystem with '...'
2. Mount with '...'
3. Run command '...'
4. See error

**Expected Behavior**
What you expected to happen.

**Environment**
- OS: [e.g., Ubuntu 22.04]
- FUSE version: [e.g., 3.14]
- Compiler: [e.g., GCC 11.4]

**Additional Context**
Any other relevant information.
```

## Feature Requests

### Before Requesting

- Check if the feature already exists
- Search existing issues and discussions
- Consider if it fits the project scope

### Feature Request Template

```markdown
**Problem**
Description of the problem this feature would solve.

**Proposed Solution**
How you envision the feature working.

**Alternatives Considered**
Other approaches you've thought about.

**Additional Context**
Any other relevant information.
```

## Questions?

If you have questions about contributing, feel free to:

- Open a discussion on GitHub
- Reach out to the maintainers
- Ask in the issue tracker

Thank you for contributing to LSFS!
