# ==============================================================================
# Makefile — RISC-V UART Communication Tool
# ==============================================================================
#
# Build targets:
#   make            Build release binary (optimized, no debug symbols)
#   make debug      Build debug binary (sanitizers, debug logging enabled)
#   make clean      Remove all build artifacts
#   make test       Run loopback test using virtual PTY pair (requires socat)
#   make help       Show available targets
#
# The compiler flags are deliberately strict (-Wall -Wextra -Wpedantic -Werror)
# to catch potential issues at compile time rather than at runtime on a
# RISC-V board where debugging is more difficult.
#
# ==============================================================================

# Compiler and standard
CC       := gcc
CSTD     := -std=c11

# POSIX feature test macro — enables sigaction(), cfmakeraw(), etc.
# 200809L corresponds to POSIX.1-2008, which includes all APIs we use.
DEFINES  := -D_POSIX_C_SOURCE=200809L

# Warning flags — maximum strictness to catch bugs early
WARNINGS := -Wall -Wextra -Wpedantic -Werror -Wshadow -Wformat=2 \
            -Wconversion -Wnull-dereference -Wdouble-promotion

# Source files
SRCDIR   := src
SOURCES  := $(SRCDIR)/main.c $(SRCDIR)/uart.c
HEADERS  := $(SRCDIR)/uart.h $(SRCDIR)/utils.h

# Output
TARGET   := uart_tool

# Release build flags
CFLAGS_RELEASE := $(CSTD) $(DEFINES) $(WARNINGS) -O2

# Debug build flags — includes AddressSanitizer for memory safety checking
# and enables LOG_DEBUG() output via -DDEBUG
CFLAGS_DEBUG   := $(CSTD) $(DEFINES) $(WARNINGS) -g -O0 -DDEBUG \
                  -fsanitize=address -fsanitize=undefined

# ==============================================================================
# Targets
# ==============================================================================

.PHONY: all debug clean test help

# Default target: release build
all: $(TARGET)
	@echo ""
	@echo "  Build complete: ./$(TARGET)"
	@echo "  Run './$(TARGET) --help' for usage information."
	@echo ""

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS_RELEASE) -o $@ $(SOURCES)

# Debug build with sanitizers and verbose logging
debug: $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS_DEBUG) -o $(TARGET)_debug $(SOURCES)
	@echo ""
	@echo "  Debug build complete: ./$(TARGET)_debug"
	@echo "  AddressSanitizer and UBSan enabled."
	@echo ""

# Loopback test using virtual PTY pair
# Requires: socat (install via: sudo apt install socat)
test: $(TARGET)
	@echo "Running loopback test..."
	@bash tests/test_loopback.sh ./$(TARGET)

# Remove build artifacts
clean:
	rm -f $(TARGET) $(TARGET)_debug
	@echo "  Clean complete."

# Show available targets
help:
	@echo ""
	@echo "  RISC-V UART Tool — Build Targets"
	@echo "  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
	@echo "  make          Release build (optimized)"
	@echo "  make debug    Debug build (sanitizers + verbose logging)"
	@echo "  make clean    Remove build artifacts"
	@echo "  make test     Run loopback test (requires socat)"
	@echo "  make help     Show this help"
	@echo ""
