# RISC-V UART Communication Tool

A professional-grade UART communication tool for Linux, built using the `termios` API. Designed in the context of **RISC-V M-Mode firmware validation** and **ACT (Architecture Compliance Test) framework** enablement on hardware boards.

## Why UART Matters for RISC-V

In RISC-V systems, UART is the primary interface between the host machine and the hardware board:

```
┌─────────────────┐     UART (TX/RX)      ┌──────────────────────┐
│  Host Machine   │◄───────────────────►  │  RISC-V Board (DUT)  │
│                 │    /dev/ttyUSB0       │                      │
│  uart_tool      │                       │  M-Mode Firmware     │
│  RISCOF         │                       │  ACT Test Binaries   │
│  OpenOCD        │                       │  Boot Loader         │
└─────────────────┘                       └──────────────────────┘
```

**M-Mode firmware** (the lowest privilege level on RISC-V) initializes UART early in the boot process to provide:
1. **Console output** — Boot logs and diagnostic messages
2. **Test signature extraction** — ACT compliance tests write their results (signatures) to UART
3. **Command interface** — Host tools send commands to control test execution

This tool demonstrates the host-side UART handling required for these workflows.

## Architecture

```
src/
├── main.c      Entry point, CLI parsing, signal handling, TX/RX orchestration
├── uart.h      Public API (HAL-style interface)
├── uart.c      Implementation: termios config, poll()-based I/O, error handling
└── utils.h     Logging macros (colored, timestamped) and hex dump utility
```

The code follows the **Hardware Abstraction Layer (HAL)** pattern used in real embedded firmware, where hardware-specific code (`uart.c`) is encapsulated behind a portable API (`uart.h`).

## Features

| Feature | Implementation |
|---|---|
| UART Configuration | `termios` API with read-back verification |
| Data Transmission | Loop-based `write()` with partial-write handling + `tcdrain()` |
| Data Reception | `poll()` with configurable timeout (non-blocking I/O) |
| Error Handling | Specific error codes, `errno` introspection, actionable messages |
| Signal Safety | `sigaction()`-based Ctrl+C handling with graceful cleanup |
| Device Safety | `TIOCEXCL` exclusive lock, original termios restoration on close |
| CLI Interface | `getopt_long()` with full parameter control |
| Debug Output | Timestamped logs, hex dump, ANSI-colored severity levels |
| Testing | Virtual PTY loopback via `socat` (no hardware required) |

## Build Instructions

### Prerequisites

- **GCC** (or any C11-compliant compiler)
- **Linux** (kernel 2.6+ for full termios support)
- **socat** (optional, for loopback testing)

```bash
# Install build tools (Arch Linux)
sudo pacman -S gcc make socat

# Install build tools (Ubuntu/Debian)
sudo apt install gcc make socat
```

### Building

```bash
# Clone/download the project
cd riscv/

# Release build (optimized, strict warnings)
make

# Debug build (AddressSanitizer + UBSan + verbose logging)
make debug

# Clean build artifacts
make clean

# Show all targets
make help
```

The release build uses strict compiler flags to catch issues at compile time:
```
-Wall -Wextra -Wpedantic -Werror -Wshadow -Wformat=2
-Wconversion -Wnull-dereference -Wdouble-promotion
```

## Usage

### Basic Usage

```bash
# Transmit a message (default: 115200 baud, 8N1)
./uart_tool -d /dev/ttyUSB0 -w "Hello RISC-V"

# Custom configuration
./uart_tool -d /dev/ttyS0 -b 9600 -p even -s 2 -w "Test"

# Verbose mode (shows debug logging)
./uart_tool -d /dev/ttyUSB0 -v -w "Debug test"
```

### All Options

```
Usage: uart_tool [OPTIONS]

Options:
  -d, --device PATH     Serial device path      (default: /dev/ttyUSB0)
  -b, --baud RATE       Baud rate                (default: 115200)
  -p, --parity TYPE     Parity: none|even|odd    (default: none)
  -s, --stop BITS       Stop bits: 1|2           (default: 1)
  -n, --databits BITS   Data bits: 5|6|7|8       (default: 8)
  -w, --write MSG       Message to transmit
  -t, --timeout MS      Read timeout in ms       (default: 5000)
  -v, --verbose         Enable debug output
  -h, --help            Show help
```

### Permission Issues

Serial ports typically require `dialout` group membership:
```bash
# Add your user to the dialout group
sudo usermod -aG dialout $USER

# Log out and back in for the change to take effect
```

## Testing Without Hardware

The project includes a loopback test that creates a virtual serial port pair using `socat`:

```bash
# Run the automated loopback test
make test
```

This creates two linked pseudo-terminals (`/tmp/vpty_riscv_a` ↔ `/tmp/vpty_riscv_b`), starts an echo responder on one end, and runs `uart_tool` on the other. Data sent through one PTY is echoed back through the other, verifying the full TX/RX path.

**Example output:**
```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  RISC-V UART Loopback Test
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
[OK] Prerequisites satisfied.
[STEP 1] Creating virtual serial port pair...
[OK] Virtual PTY pair created
[STEP 2] Starting echo responder...
[STEP 3] Running uart_tool...

[TX] Sending message (45 bytes):
  "RISC-V ACT Loopback Test: 0x5A 0xA5 0xDE 0xAD"
  ✓ 45 bytes transmitted successfully

[RX] Received 45 bytes:
  ASCII: "RISC-V ACT Loopback Test: 0x5A 0xA5 0xDE 0xAD"
  Hex dump:
  00000000  52 49 53 43 2d 56 20 41  43 54 20 4c 6f 6f 70 62  |RISC-V ACT Loopb|
  00000010  61 63 6b 20 54 65 73 74  3a 20 30 78 35 41 20 30  |ack Test: 0x5A 0|
  00000020  78 41 35 20 30 78 44 45  20 30 78 41 44           |xA5 0xDE 0xAD|

  ✓ LOOPBACK TEST PASSED
```

This approach mirrors the ACT framework philosophy where tests must work on both simulators and real hardware.

## Error Handling

The program handles errors at every level with specific, actionable messages:

| Error Scenario | Detection | Message |
|---|---|---|
| Device not found | `errno == ENOENT` | "Device not found — check connection" |
| Permission denied | `errno == EACCES` | "Permission denied — add to dialout group" |
| Not a serial port | `isatty()` fails | "Not a TTY device" |
| Unsupported baud rate | Lookup table miss | "Unsupported baud rate: N" |
| Config not applied | `tcsetattr` read-back | "Hardware did not accept configuration" |
| Partial write | `write()` < requested | Automatic retry loop |
| Read timeout | `poll()` returns 0 | Graceful exit after 3 consecutive timeouts |
| Device disconnected | `POLLHUP` event | "USB adapter may have been unplugged" |
| Signal interrupt | `SIGINT`/`SIGTERM` | Clean shutdown, restore termios |

## Design Decisions

### Why `poll()` over `select()`?
- `select()` is limited to `FD_SETSIZE` (typically 1024) file descriptors
- `select()` modifies its fd sets, requiring reconstruction each iteration
- `poll()` uses a cleaner `pollfd` struct with separate `events`/`revents` fields
- `poll()` is the recommended API for modern Linux serial programming

### Why `cfmakeraw()`?
Raw mode disables all line discipline processing (echo, canonical mode, signal generation, CR/LF translation). This is essential for firmware communication where every byte — including control characters — must pass through unmodified.

### Why exclusive lock (`TIOCEXCL`)?
On Linux, background services like ModemManager may probe serial ports and inject AT commands, corrupting communication. `TIOCEXCL` prevents other processes from opening the device.

### Why restore original termios?
The serial port is a shared resource. Restoring original settings ensures other tools (minicom, screen, OpenOCD) can use the port after our program exits.

## License

MIT License — see source file headers for details.
