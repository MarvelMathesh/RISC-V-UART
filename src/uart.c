/*
 * =============================================================================
 * uart.c — UART Implementation using Linux termios API
 * =============================================================================
 *
 * Implements the UART HAL defined in uart.h. This file contains all
 * hardware-facing code: device opening, termios configuration, data
 * transmission/reception via poll(), and cleanup.
 *
 * Key Implementation Decisions:
 *   - Raw mode (cfmakeraw) for binary-safe communication
 *   - poll() over select() for I/O multiplexing (no FD_SETSIZE limit)
 *   - Exclusive device lock (TIOCEXCL) to prevent interference
 *   - Read-back verification of termios settings
 *   - Partial write handling in a retry loop
 *   - Original settings restored on close
 *
 * Author:  Mathesh
 * License: MIT
 * =============================================================================
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "uart.h"
#include "utils.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* ---------------------------------------------------------------------------
 * Module-level State
 *
 * We save the original termios configuration so we can restore it when
 * closing the port. This ensures the device is left in a clean state for
 * other applications (e.g., minicom, OpenOCD, or another test run).
 *
 * 'original_saved' guards against restoring uninitialized data if
 * uart_close() is called without a successful uart_open().
 * ---------------------------------------------------------------------------
 */
static struct termios original_termios;
static int original_saved = 0;

/* ---------------------------------------------------------------------------
 * Baud Rate Mapping
 *
 * Linux termios uses symbolic constants (B9600, B115200, etc.) rather than
 * raw integer baud rates. This table maps user-supplied integer values to
 * the corresponding termios constants.
 *
 * The rates listed here cover the full range commonly used with RISC-V
 * boards — from 9600 (legacy sensors) through 115200 (standard debug
 * console) up to 4000000 (high-speed trace output).
 * ---------------------------------------------------------------------------
 */
typedef struct {
    unsigned int rate;
    speed_t      symbol;
} baud_entry_t;

static const baud_entry_t baud_table[] = {
    {      1200, B1200   },
    {      2400, B2400   },
    {      4800, B4800   },
    {      9600, B9600   },
    {     19200, B19200  },
    {     38400, B38400  },
    {     57600, B57600  },
    {    115200, B115200 },
    {    230400, B230400 },
    {    460800, B460800 },
    {    500000, B500000 },
    {    576000, B576000 },
    {    921600, B921600 },
    {   1000000, B1000000},
    {   1152000, B1152000},
    {   1500000, B1500000},
    {   2000000, B2000000},
    {   2500000, B2500000},
    {   3000000, B3000000},
    {   3500000, B3500000},
    {   4000000, B4000000},
};

#define BAUD_TABLE_SIZE (sizeof(baud_table) / sizeof(baud_table[0]))

/*
 * lookup_baud — Convert integer baud rate to termios speed constant.
 * Returns -1 if the rate is not in the supported table.
 */
static speed_t lookup_baud(unsigned int rate)
{
    for (size_t i = 0; i < BAUD_TABLE_SIZE; i++) {
        if (baud_table[i].rate == rate) {
            return baud_table[i].symbol;
        }
    }
    return (speed_t)-1;
}

/* ---------------------------------------------------------------------------
 * uart_config_default
 * ---------------------------------------------------------------------------
 */
uart_config_t uart_config_default(void)
{
    uart_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.device_path = "/dev/ttyUSB0";
    cfg.baud_rate   = 115200;        /* Standard for RISC-V debug consoles */
    cfg.data_bits   = 8;
    cfg.stop_bits   = 1;
    cfg.parity      = UART_PARITY_NONE;
    cfg.timeout_ms  = 5000;          /* 5-second default read timeout      */
    cfg.verbose     = 0;

    return cfg;
}

/* ---------------------------------------------------------------------------
 * configure_termios — Apply UART parameters to the termios structure
 *
 * This is the core configuration function. It translates the user-facing
 * uart_config_t into the low-level termios bit fields that the Linux
 * serial driver uses to program the UART hardware registers.
 *
 * The function follows the "read-modify-write" pattern:
 *   1. Read current settings with tcgetattr()
 *   2. Apply cfmakeraw() for binary mode
 *   3. Set specific flags for baud, data bits, parity, stop bits
 *   4. Write settings with tcsetattr()
 *   5. Read back and verify the settings took effect
 *
 * Step 5 is critical because tcsetattr() can silently succeed even if
 * the hardware doesn't support the requested configuration (per POSIX).
 * ---------------------------------------------------------------------------
 */
static int configure_termios(int fd, const uart_config_t *config)
{
    struct termios tty;

    /* Step 1: Read current terminal attributes */
    if (tcgetattr(fd, &tty) != 0) {
        LOG_ERROR("tcgetattr() failed: %s", strerror(errno));
        return UART_ERR_CONFIG;
    }

    /*
     * Step 2: Set raw mode
     *
     * cfmakeraw() disables all line discipline processing:
     *   - No echo, no canonical mode (line editing)
     *   - No signal generation (Ctrl+C won't send SIGINT)
     *   - No output processing (no CR-to-LF translation)
     *   - Input is available byte-by-byte, not line-by-line
     *
     * This is essential for firmware communication where every byte
     * (including control characters) must pass through unmodified.
     */
    cfmakeraw(&tty);

    /*
     * Step 3a: Configure baud rate
     *
     * cfsetispeed/cfsetospeed set the input and output baud rates
     * independently, though they are almost always the same for
     * standard UART (which is full-duplex at a single rate).
     */
    speed_t baud_sym = lookup_baud(config->baud_rate);
    if (baud_sym == (speed_t)-1) {
        LOG_ERROR("Unsupported baud rate: %u", config->baud_rate);
        return UART_ERR_BAUD;
    }

    if (cfsetispeed(&tty, baud_sym) != 0 ||
        cfsetospeed(&tty, baud_sym) != 0) {
        LOG_ERROR("cfsetspeed() failed for %u baud: %s",
                  config->baud_rate, strerror(errno));
        return UART_ERR_BAUD;
    }

    /*
     * Step 3b: Configure data bits (character size)
     *
     * CSIZE is a bit mask covering CS5, CS6, CS7, CS8.
     * We clear it first, then set the requested size.
     * 8-bit is standard for modern UART; 7-bit was used historically
     * for ASCII-only protocols.
     */
    tty.c_cflag &= (tcflag_t)~CSIZE;
    switch (config->data_bits) {
        case 5: tty.c_cflag |= CS5; break;
        case 6: tty.c_cflag |= CS6; break;
        case 7: tty.c_cflag |= CS7; break;
        case 8: tty.c_cflag |= CS8; break;
        default:
            LOG_ERROR("Invalid data bits: %d (must be 5-8)", config->data_bits);
            return UART_ERR_PARAM;
    }

    /*
     * Step 3c: Configure parity
     *
     * PARENB enables parity generation/checking.
     * PARODD selects odd parity (when PARENB is set).
     * If neither is set, no parity bit is transmitted.
     */
    tty.c_cflag &= (tcflag_t)~(PARENB | PARODD);
    switch (config->parity) {
        case UART_PARITY_NONE:
            /* Parity bits already cleared above */
            break;
        case UART_PARITY_EVEN:
            tty.c_cflag |= PARENB;
            break;
        case UART_PARITY_ODD:
            tty.c_cflag |= (PARENB | PARODD);
            break;
    }

    /*
     * Step 3d: Configure stop bits
     *
     * CSTOPB flag: if set, 2 stop bits; if clear, 1 stop bit.
     * Most configurations use 1 stop bit. 2 stop bits are sometimes
     * used at lower baud rates for more reliable synchronization.
     */
    if (config->stop_bits == 2) {
        tty.c_cflag |= CSTOPB;
    } else {
        tty.c_cflag &= (tcflag_t)~CSTOPB;
    }

    /*
     * Step 3e: Additional control flags
     *
     * CLOCAL: Ignore modem control lines (DCD, DSR, CTS, RTS).
     *         Essential for direct UART connections without a modem.
     *         Most RISC-V boards connect UART directly without
     *         hardware flow control signals.
     *
     * CREAD:  Enable the receiver. Without this, the port is TX-only.
     */
    tty.c_cflag |= (CLOCAL | CREAD);

    /*
     * Step 3f: Disable hardware and software flow control
     *
     * CRTSCTS: Hardware flow control using RTS/CTS pins.
     *          Disabled because most RISC-V debug UARTs only have
     *          TX/RX pins (no flow control lines).
     *
     * IXON/IXOFF/IXANY: Software flow control using XON/XOFF characters.
     *          Disabled to prevent binary data containing 0x11 (XON) or
     *          0x13 (XOFF) from being interpreted as flow control commands.
     */
    tty.c_cflag &= (tcflag_t)~CRTSCTS;
    tty.c_iflag &= (tcflag_t)~(IXON | IXOFF | IXANY);

    /*
     * Step 3g: Configure VMIN and VTIME for non-canonical mode
     *
     * VMIN = 0, VTIME = 1:
     *   read() returns immediately when at least 1 byte is available,
     *   or returns 0 after 100ms (VTIME is in deciseconds) if no data.
     *   This provides a basic polling mechanism at the read() level,
     *   complementing our poll()-based timeout at the application level.
     *
     * Using both poll() and VMIN/VTIME provides defense-in-depth:
     *   poll() handles the macro-level timeout (seconds),
     *   VMIN/VTIME prevents read() from blocking indefinitely if poll()
     *   reports data available but the data is consumed between poll()
     *   returning and read() being called (a subtle race condition).
     */
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;

    /*
     * Step 4: Flush stale data and apply settings
     *
     * tcflush() clears any data left in the kernel's serial buffers
     * from previous sessions. Without this, you might read stale data
     * that was buffered before your program started.
     *
     * TCSANOW applies changes immediately (vs TCSADRAIN which waits
     * for output to complete, or TCSAFLUSH which also flushes input).
     */
    tcflush(fd, TCIOFLUSH);

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        LOG_ERROR("tcsetattr() failed: %s", strerror(errno));
        return UART_ERR_CONFIG;
    }

    /*
     * Step 5: Verify settings were applied correctly
     *
     * POSIX allows tcsetattr() to succeed even if only some settings
     * were applied. We read back the actual settings and verify the
     * critical parameters match what we requested.
     */
    struct termios verify;
    if (tcgetattr(fd, &verify) != 0) {
        LOG_ERROR("tcgetattr() verification failed: %s", strerror(errno));
        return UART_ERR_CONFIG;
    }

    if (cfgetispeed(&verify) != baud_sym) {
        LOG_ERROR("Baud rate verification failed: requested %u, hardware did not accept",
                  config->baud_rate);
        return UART_ERR_CONFIG;
    }

    LOG_DEBUG("termios configuration verified successfully");
    return UART_OK;
}

/* ---------------------------------------------------------------------------
 * uart_open
 * ---------------------------------------------------------------------------
 */
int uart_open(const uart_config_t *config)
{
    /* Validate parameters */
    if (config == NULL || config->device_path == NULL) {
        LOG_ERROR("NULL configuration or device path");
        return UART_ERR_PARAM;
    }

    LOG_INFO("Opening UART device: %s", config->device_path);
    LOG_INFO("Configuration: %u baud, %d%c%d",
             config->baud_rate,
             config->data_bits,
             config->parity == UART_PARITY_NONE ? 'N' :
             config->parity == UART_PARITY_EVEN ? 'E' : 'O',
             config->stop_bits);

    /*
     * Open the device file.
     *
     * O_RDWR:     We need both read and write access for bidirectional UART.
     *
     * O_NOCTTY:   Prevents this serial port from becoming the process's
     *             controlling terminal. Without this, input from the serial
     *             port could generate signals (e.g., Ctrl+C → SIGINT).
     *
     * O_NONBLOCK: Makes open() return immediately even if DCD (Data Carrier
     *             Detect) is not asserted. Some USB-to-serial adapters
     *             block on open() without this flag. We'll configure the
     *             actual read behavior separately via poll() and VMIN/VTIME.
     */
    int fd = open(config->device_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        if (errno == EACCES) {
            LOG_ERROR("Permission denied: %s — try running with sudo or "
                      "add your user to the 'dialout' group: "
                      "sudo usermod -aG dialout $USER",
                      config->device_path);
        } else if (errno == ENOENT) {
            LOG_ERROR("Device not found: %s — check if the device is "
                      "connected and the path is correct",
                      config->device_path);
        } else {
            LOG_ERROR("Failed to open %s: %s",
                      config->device_path, strerror(errno));
        }
        return UART_ERR_OPEN;
    }

    /*
     * Verify the file descriptor refers to a real terminal device.
     * This catches mistakes like passing a regular file or /dev/null.
     */
    if (!isatty(fd)) {
        LOG_ERROR("%s is not a TTY device (isatty failed: %s)",
                  config->device_path, strerror(errno));
        close(fd);
        return UART_ERR_NOT_TTY;
    }

    /*
     * Acquire exclusive access to the serial port.
     *
     * TIOCEXCL prevents other processes from opening this device.
     * This is critical on Linux where background services (especially
     * ModemManager) may probe serial ports and inject AT commands,
     * corrupting UART communication with the RISC-V board.
     */
    if (ioctl(fd, TIOCEXCL) != 0) {
        LOG_ERROR("Failed to set exclusive mode on %s: %s "
                  "(another process may have the port open)",
                  config->device_path, strerror(errno));
        close(fd);
        return UART_ERR_EXCLUSIVE;
    }

    /*
     * Save original terminal settings for restoration on close.
     * This ensures we leave the port in a clean state — important
     * when sharing the port with tools like minicom or OpenOCD.
     */
    if (tcgetattr(fd, &original_termios) != 0) {
        LOG_ERROR("Failed to save original termios: %s", strerror(errno));
        close(fd);
        return UART_ERR_CONFIG;
    }
    original_saved = 1;

    /* Apply the requested UART configuration */
    int ret = configure_termios(fd, config);
    if (ret != UART_OK) {
        close(fd);
        return ret;
    }

    LOG_INFO("UART device opened and configured successfully (fd=%d)", fd);
    return fd;
}

/* ---------------------------------------------------------------------------
 * uart_write_data
 * ---------------------------------------------------------------------------
 */
ssize_t uart_write_data(int fd, const void *data, size_t len)
{
    if (data == NULL || len == 0) {
        LOG_ERROR("uart_write_data: NULL data or zero length");
        return UART_ERR_PARAM;
    }

    const unsigned char *buf = (const unsigned char *)data;
    size_t total_written = 0;

    LOG_DEBUG("Transmitting %zu bytes...", len);

    /*
     * Write loop: handle partial writes.
     *
     * A single write() call may not transmit all bytes if:
     *   - The kernel's output buffer is full
     *   - Hardware flow control (RTS/CTS) is stalling transmission
     *   - The write is interrupted by a signal (EINTR)
     *
     * We retry until all bytes are sent or a fatal error occurs.
     */
    while (total_written < len) {
        ssize_t n = write(fd, buf + total_written, len - total_written);

        if (n < 0) {
            if (errno == EINTR) {
                /* Interrupted by signal — retry immediately */
                LOG_DEBUG("write() interrupted by signal, retrying...");
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /*
                 * Output buffer full — wait briefly and retry.
                 * This happens with non-blocking I/O when the hardware
                 * hasn't finished transmitting previous data.
                 */
                LOG_DEBUG("Output buffer full, waiting 10ms...");
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 };
                nanosleep(&ts, NULL);
                continue;
            }
            /* Unrecoverable write error */
            LOG_ERROR("write() failed after %zu/%zu bytes: %s",
                      total_written, len, strerror(errno));
            return UART_ERR_WRITE;
        }

        total_written += (size_t)n;
        LOG_DEBUG("Wrote %zd bytes (%zu/%zu total)", n, total_written, len);
    }

    /*
     * tcdrain() blocks until all output has been physically transmitted.
     *
     * Without this, closing the port immediately after write() could
     * discard buffered data that hasn't been clocked out yet. This is
     * especially important at lower baud rates where transmission of
     * even a short message takes measurable time.
     */
    if (tcdrain(fd) != 0) {
        LOG_ERROR("tcdrain() failed: %s (data may not be fully transmitted)",
                  strerror(errno));
    }

    LOG_INFO("Transmitted %zu bytes successfully", total_written);
    return (ssize_t)total_written;
}

/* ---------------------------------------------------------------------------
 * uart_read_data
 * ---------------------------------------------------------------------------
 */
ssize_t uart_read_data(int fd, void *buf, size_t buf_size, int timeout_ms)
{
    if (buf == NULL || buf_size == 0) {
        LOG_ERROR("uart_read_data: NULL buffer or zero size");
        return UART_ERR_PARAM;
    }

    /*
     * poll() — Wait for data with timeout.
     *
     * We use poll() instead of select() because:
     *   1. No FD_SETSIZE limitation (select is capped at 1024 fds)
     *   2. Cleaner API: pollfd struct vs. three separate fd_set bitmasks
     *   3. No need to recalculate the highest fd number
     *   4. The fd set doesn't need to be reconstructed each iteration
     *
     * POLLIN:  There is data to read (normal data or priority data).
     * POLLERR: An error condition on the device (checked in revents).
     * POLLHUP: The device has been disconnected (e.g., USB unplugged).
     * POLLNVAL: The fd is not open (programming error).
     */
    struct pollfd pfd;
    pfd.fd      = fd;
    pfd.events  = POLLIN;
    pfd.revents = 0;

    LOG_DEBUG("Waiting for data (timeout: %dms)...", timeout_ms);

    int poll_ret = poll(&pfd, 1, timeout_ms);

    if (poll_ret < 0) {
        if (errno == EINTR) {
            LOG_INFO("poll() interrupted by signal");
            return 0;
        }
        LOG_ERROR("poll() failed: %s", strerror(errno));
        return UART_ERR_POLL;
    }

    if (poll_ret == 0) {
        /* Timeout — no data arrived within the specified window */
        LOG_DEBUG("Read timeout after %dms (no data received)", timeout_ms);
        return UART_ERR_TIMEOUT;
    }

    /* Check for error conditions reported by poll() */
    if (pfd.revents & POLLERR) {
        LOG_ERROR("Device error detected (POLLERR) — possible hardware issue");
        return UART_ERR_READ;
    }
    if (pfd.revents & POLLHUP) {
        LOG_ERROR("Device disconnected (POLLHUP) — USB adapter may have been unplugged");
        return UART_ERR_READ;
    }
    if (pfd.revents & POLLNVAL) {
        LOG_ERROR("Invalid file descriptor (POLLNVAL) — port may be closed");
        return UART_ERR_READ;
    }

    /* Data is available — read it */
    if (pfd.revents & POLLIN) {
        ssize_t n = read(fd, buf, buf_size);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Spurious wake-up — data was consumed between poll and read */
                LOG_DEBUG("Spurious poll wake-up (EAGAIN), no data read");
                return 0;
            }
            LOG_ERROR("read() failed: %s", strerror(errno));
            return UART_ERR_READ;
        }

        LOG_DEBUG("Received %zd bytes", n);
        return n;
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * uart_close
 * ---------------------------------------------------------------------------
 */
void uart_close(int fd)
{
    if (fd < 0) {
        return;
    }

    /*
     * Restore original terminal settings.
     *
     * This is good citizenship — it ensures the serial port is left
     * in the state it was in before our program modified it. This
     * matters when other tools (minicom, screen, OpenOCD) will use
     * the same port after us.
     */
    if (original_saved) {
        if (tcsetattr(fd, TCSANOW, &original_termios) != 0) {
            LOG_ERROR("Failed to restore original termios: %s",
                      strerror(errno));
        } else {
            LOG_DEBUG("Original terminal settings restored");
        }
        original_saved = 0;
    }

    if (close(fd) != 0) {
        LOG_ERROR("close() failed: %s", strerror(errno));
    } else {
        LOG_INFO("UART device closed (fd=%d)", fd);
    }
}

/* ---------------------------------------------------------------------------
 * uart_strerror
 * ---------------------------------------------------------------------------
 */
const char *uart_strerror(uart_error_t err)
{
    switch (err) {
        case UART_OK:            return "Success";
        case UART_ERR_OPEN:      return "Failed to open device";
        case UART_ERR_NOT_TTY:   return "Device is not a terminal";
        case UART_ERR_CONFIG:    return "Configuration failed";
        case UART_ERR_BAUD:      return "Unsupported baud rate";
        case UART_ERR_WRITE:     return "Write failed";
        case UART_ERR_READ:      return "Read failed";
        case UART_ERR_TIMEOUT:   return "Read timed out";
        case UART_ERR_POLL:      return "poll() failed";
        case UART_ERR_EXCLUSIVE: return "Exclusive lock failed";
        case UART_ERR_PARAM:     return "Invalid parameter";
        default:                 return "Unknown error";
    }
}
