#!/bin/bash
# ==============================================================================
# test_loopback.sh — Automated Loopback Test using Virtual PTY Pair
# ==============================================================================
#
# This script creates a virtual serial port pair using socat, then runs the
# UART tool against one end while echoing data from the other end. This allows
# testing the full TX/RX path without physical hardware.
#
# Why this matters for RISC-V development:
#   The ACT (Architecture Compliance Test) framework must run on both
#   simulators and real hardware. Being able to test UART communication
#   without a physical board is essential for CI/CD pipelines and
#   development on machines without connected hardware.
#
# Prerequisites:
#   - socat (install: sudo apt install socat)
#
# Usage:
#   ./tests/test_loopback.sh ./uart_tool
#
# ==============================================================================

set -e

# Colors for output
RED='\033[1;31m'
GREEN='\033[1;32m'
YELLOW='\033[1;33m'
BLUE='\033[1;34m'
NC='\033[0m'

# The uart_tool binary path (passed as argument or default)
UART_TOOL="${1:-./uart_tool}"

# Virtual PTY paths
PTY_A="/tmp/vpty_riscv_a"
PTY_B="/tmp/vpty_riscv_b"

# Test message
TEST_MSG="RISC-V ACT Loopback Test: 0x5A 0xA5 0xDE 0xAD"

# PID tracking for cleanup
SOCAT_PID=""
ECHO_PID=""

# ---------------------------------------------------------------------------
# Cleanup function — ensures background processes are killed on exit
# ---------------------------------------------------------------------------
cleanup() {
    echo -e "\n${BLUE}[CLEANUP]${NC} Stopping background processes..."
    [ -n "$ECHO_PID" ] && kill "$ECHO_PID" 2>/dev/null || true
    [ -n "$SOCAT_PID" ] && kill "$SOCAT_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    rm -f "$PTY_A" "$PTY_B"
    echo -e "${BLUE}[CLEANUP]${NC} Done."
}

trap cleanup EXIT

# ---------------------------------------------------------------------------
# Preflight checks
# ---------------------------------------------------------------------------
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}  RISC-V UART Loopback Test${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"

# Check for socat
if ! command -v socat &>/dev/null; then
    echo -e "${RED}[ERROR]${NC} socat is not installed."
    echo -e "  Install it with: ${YELLOW}sudo apt install socat${NC}"
    exit 1
fi

# Check for uart_tool binary
if [ ! -x "$UART_TOOL" ]; then
    echo -e "${RED}[ERROR]${NC} UART tool not found at: $UART_TOOL"
    echo -e "  Build it first with: ${YELLOW}make${NC}"
    exit 1
fi

echo -e "${GREEN}[OK]${NC} Prerequisites satisfied."

# ---------------------------------------------------------------------------
# Step 1: Create virtual PTY pair
# ---------------------------------------------------------------------------
echo -e "\n${YELLOW}[STEP 1]${NC} Creating virtual serial port pair..."

# socat creates two linked pseudo-terminals.
# Data written to PTY_A appears as input on PTY_B and vice versa.
# This simulates a physical UART loopback cable.
socat -d -d \
    "pty,raw,echo=0,link=$PTY_A" \
    "pty,raw,echo=0,link=$PTY_B" &
SOCAT_PID=$!

# Wait for PTY devices to appear
sleep 1

if [ ! -e "$PTY_A" ] || [ ! -e "$PTY_B" ]; then
    echo -e "${RED}[ERROR]${NC} Failed to create virtual PTY pair."
    exit 1
fi

echo -e "${GREEN}[OK]${NC} Virtual PTY pair created:"
echo -e "  PTY A (uart_tool): $PTY_A"
echo -e "  PTY B (echo side): $PTY_B"

# ---------------------------------------------------------------------------
# Step 2: Start echo responder on PTY_B
# ---------------------------------------------------------------------------
echo -e "\n${YELLOW}[STEP 2]${NC} Starting echo responder on $PTY_B..."

# This background process reads from PTY_B and writes it back,
# simulating a RISC-V board that echoes received data.
(cat < "$PTY_B" > "$PTY_B") &
ECHO_PID=$!

sleep 0.5
echo -e "${GREEN}[OK]${NC} Echo responder running (PID: $ECHO_PID)."

# ---------------------------------------------------------------------------
# Step 3: Run UART tool with test message
# ---------------------------------------------------------------------------
echo -e "\n${YELLOW}[STEP 3]${NC} Running uart_tool..."
echo -e "  Device:  $PTY_A"
echo -e "  Message: \"$TEST_MSG\""
echo -e ""

# Run the uart tool — it will transmit the message, then try to receive
# the echo back from PTY_B
if "$UART_TOOL" -d "$PTY_A" -b 115200 -w "$TEST_MSG" -t 2000; then
    echo -e "\n${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${GREEN}  ✓ LOOPBACK TEST PASSED${NC}"
    echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    exit 0
else
    EXIT_CODE=$?
    echo -e "\n${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${RED}  ✗ LOOPBACK TEST FAILED (exit code: $EXIT_CODE)${NC}"
    echo -e "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    exit 1
fi
