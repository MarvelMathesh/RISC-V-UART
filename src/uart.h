/*
 * =============================================================================
 * uart.h — UART Hardware Abstraction Layer (Public API)
 * =============================================================================
 *
 * Provides a clean, reusable interface for UART communication on Linux.
 * This header defines the public API that isolates application logic from
 * the underlying termios implementation details.
 *
 * Architecture Context:
 *   In RISC-V systems, UART is the primary debug interface. M-Mode firmware
 *   (the lowest privilege level) typically initializes UART early in the boot
 *   process to provide console output. The RISC-V ACT (Architecture Compliance
 *   Test) framework uses UART to:
 *     1. Extract test signatures from the DUT (Device Under Test)
 *     2. Capture boot logs and diagnostic output
 *     3. Send commands to control test execution on hardware boards
 *
 *   This module mirrors the HAL pattern used in real embedded firmware,
 *   where hardware-specific code is encapsulated behind a portable API.
 *
 * Usage:
 *   uart_config_t cfg = uart_config_default();
 *   cfg.device_path = "/dev/ttyUSB0";
 *   cfg.baud_rate   = 115200;
 *
 *   int fd = uart_open(&cfg);
 *   if (fd < 0) { handle error }
 *
 *   uart_configure(fd, &cfg);
 *   uart_write_data(fd, "Hello RISC-V\n", 13);
 *
 *   char buf[256];
 *   ssize_t n = uart_read_data(fd, buf, sizeof(buf), 5000);
 *
 *   uart_close(fd);
 *
 * Author:  Mathesh
 * License: MIT
 * =============================================================================
 */

#ifndef UART_H
#define UART_H

#include <stddef.h>     /* size_t                     */
#include <sys/types.h>  /* ssize_t                    */

/* ---------------------------------------------------------------------------
 * Error Codes
 *
 * Specific error codes allow the caller to distinguish between failure modes
 * and take appropriate action (e.g., retry on timeout, abort on permission
 * denied). This is more informative than a generic -1 return.
 *
 * Negative values are used by convention in Linux kernel/driver code.
 * ---------------------------------------------------------------------------
 */
typedef enum {
    UART_OK             =  0,    /* Operation completed successfully         */
    UART_ERR_OPEN       = -1,    /* Failed to open device file               */
    UART_ERR_NOT_TTY    = -2,    /* Device is not a terminal/serial port     */
    UART_ERR_CONFIG     = -3,    /* Failed to get/set termios attributes     */
    UART_ERR_BAUD       = -4,    /* Unsupported baud rate requested          */
    UART_ERR_WRITE      = -5,    /* Write operation failed                   */
    UART_ERR_READ       = -6,    /* Read operation failed                    */
    UART_ERR_TIMEOUT    = -7,    /* Read timed out (no data within window)   */
    UART_ERR_POLL       = -8,    /* poll() system call failed                */
    UART_ERR_EXCLUSIVE  = -9,    /* Failed to acquire exclusive device lock  */
    UART_ERR_PARAM      = -10    /* Invalid parameter supplied               */
} uart_error_t;

/* ---------------------------------------------------------------------------
 * Parity Configuration
 *
 * Parity is an error-detection mechanism where an extra bit is appended to
 * each data frame. In RISC-V hardware validation:
 *   - Most debug UARTs use NO parity (8N1 configuration)
 *   - Some industrial interfaces may require EVEN parity
 *   - ODD parity is less common but supported for completeness
 * ---------------------------------------------------------------------------
 */
typedef enum {
    UART_PARITY_NONE,   /* No parity bit (most common for RISC-V debug)     */
    UART_PARITY_EVEN,   /* Even parity — bit set so total 1s are even       */
    UART_PARITY_ODD     /* Odd parity  — bit set so total 1s are odd        */
} uart_parity_t;

/* ---------------------------------------------------------------------------
 * UART Configuration Structure
 *
 * Encapsulates all parameters needed to configure a serial port. Using a
 * struct rather than individual function arguments makes the API cleaner
 * and allows easy extension (e.g., adding flow control) without breaking
 * existing code.
 *
 * Default values (set by uart_config_default()) match the standard 8N1
 * configuration at 115200 baud, which is the de-facto standard for
 * RISC-V development boards and debug consoles.
 * ---------------------------------------------------------------------------
 */
typedef struct {
    const char   *device_path;   /* Device file path (e.g., "/dev/ttyUSB0") */
    unsigned int  baud_rate;     /* Baud rate (e.g., 9600, 115200, 921600)  */
    int           data_bits;     /* Data bits per frame: 5, 6, 7, or 8     */
    int           stop_bits;     /* Stop bits: 1 or 2                       */
    uart_parity_t parity;        /* Parity mode: none, even, or odd         */
    int           timeout_ms;    /* Read timeout in milliseconds             */
    int           verbose;       /* Enable verbose/debug logging             */
} uart_config_t;

/* ---------------------------------------------------------------------------
 * API Functions
 * ---------------------------------------------------------------------------
 */

/*
 * uart_config_default — Create a configuration with sensible defaults
 *
 * Returns a uart_config_t initialized to:
 *   - Device:   /dev/ttyUSB0
 *   - Baud:     115200 (standard for RISC-V boards)
 *   - Format:   8N1 (8 data bits, no parity, 1 stop bit)
 *   - Timeout:  5000ms (5 seconds)
 *   - Verbose:  disabled
 *
 * Callers should override specific fields as needed before passing
 * the config to uart_open().
 */
uart_config_t uart_config_default(void);

/*
 * uart_open — Open and initialize a UART device
 *
 * Performs the following steps:
 *   1. Validates the device_path parameter
 *   2. Opens the device with O_RDWR | O_NOCTTY | O_NONBLOCK
 *   3. Verifies the file descriptor refers to a TTY device (isatty)
 *   4. Acquires an exclusive lock (TIOCEXCL) to prevent interference
 *   5. Saves the original termios settings for later restoration
 *   6. Applies the requested UART configuration via termios
 *
 * Parameters:
 *   config — Pointer to a populated uart_config_t structure
 *
 * Returns:
 *   Non-negative file descriptor on success, or a uart_error_t code on failure
 */
int uart_open(const uart_config_t *config);

/*
 * uart_write_data — Transmit data over the UART interface
 *
 * Handles partial writes by looping until all bytes are sent or an
 * unrecoverable error occurs. Calls tcdrain() after writing to ensure
 * all data is physically transmitted before returning.
 *
 * Parameters:
 *   fd   — File descriptor from uart_open()
 *   data — Pointer to the data buffer to transmit
 *   len  — Number of bytes to transmit
 *
 * Returns:
 *   Number of bytes successfully written, or a uart_error_t code on failure
 */
ssize_t uart_write_data(int fd, const void *data, size_t len);

/*
 * uart_read_data — Receive data from the UART interface
 *
 * Uses poll() to wait for incoming data with a configurable timeout.
 * This non-blocking approach prevents the program from hanging indefinitely
 * if no data arrives (e.g., if the remote device is powered off or
 * misconfigured).
 *
 * Parameters:
 *   fd         — File descriptor from uart_open()
 *   buf        — Buffer to store received data
 *   buf_size   — Maximum number of bytes to read
 *   timeout_ms — Maximum time to wait for data, in milliseconds.
 *                Use 0 for non-blocking check, -1 for infinite wait.
 *
 * Returns:
 *   Number of bytes read on success (may be 0 if no data within timeout),
 *   or a uart_error_t code on failure
 */
ssize_t uart_read_data(int fd, void *buf, size_t buf_size, int timeout_ms);

/*
 * uart_close — Close the UART device and restore original settings
 *
 * Restores the terminal settings that were saved during uart_open(),
 * ensuring the device is left in a clean state for other applications.
 * This is particularly important when the serial port is shared with
 * other tools (e.g., minicom, screen, or OpenOCD for RISC-V debugging).
 *
 * Parameters:
 *   fd — File descriptor from uart_open()
 */
void uart_close(int fd);

/*
 * uart_strerror — Convert a uart_error_t code to a human-readable string
 *
 * Parameters:
 *   err — A uart_error_t error code
 *
 * Returns:
 *   Static string describing the error (never NULL)
 */
const char *uart_strerror(uart_error_t err);

#endif /* UART_H */
