# LSFS Makefile - Wrapper for CMake build system
#
# Usage:
#   make            - Build in release mode
#   make debug      - Build in debug mode
#   make clean      - Clean build directory
#   make test       - Run tests
#   make install    - Install to system
#   make help       - Show this help

.PHONY: all debug clean test install help format check

BUILD_DIR := build
CMAKE := cmake
MAKE_FLAGS := -j$(shell nproc)

all: release

release: $(BUILD_DIR)
	@cd $(BUILD_DIR) && $(CMAKE) -DCMAKE_BUILD_TYPE=Release .. && $(MAKE) $(MAKE_FLAGS)
	@echo ""
	@echo "Build complete. Executables are in $(BUILD_DIR)/"

debug: $(BUILD_DIR)
	@cd $(BUILD_DIR) && $(CMAKE) -DCMAKE_BUILD_TYPE=Debug .. && $(MAKE) $(MAKE_FLAGS)
	@echo ""
	@echo "Debug build complete. Executables are in $(BUILD_DIR)/"

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	@rm -rf $(BUILD_DIR)
	@echo "Build directory cleaned."

test: release
	@./scripts/test.sh

install: release
	@cd $(BUILD_DIR) && sudo $(MAKE) install
	@echo "Installation complete."

format:
	@find src include tools -name '*.c' -o -name '*.h' | xargs clang-format -i
	@echo "Code formatted."

check:
	@echo "Checking for common issues..."
	@! grep -rn --include="*.c" --include="*.h" 'TODO\|FIXME\|XXX' src/ include/ tools/ || true
	@echo "Check complete."

help:
	@echo "LSFS Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all, release  - Build in release mode (default)"
	@echo "  debug         - Build in debug mode with symbols"
	@echo "  clean         - Remove build directory"
	@echo "  test          - Run test suite"
	@echo "  install       - Install to system (requires sudo)"
	@echo "  format        - Format code with clang-format"
	@echo "  check         - Check for TODOs and issues"
	@echo "  help          - Show this help"
	@echo ""
	@echo "Examples:"
	@echo "  make                    # Build release"
	@echo "  make debug              # Build with debug symbols"
	@echo "  make test               # Run tests"
	@echo "  make clean && make      # Clean rebuild"
