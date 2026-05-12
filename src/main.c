/*
 * =============================================================================
 * main.c — RISC-V UART Communication Tool
 * =============================================================================
 *
 * Entry point for the UART communication tool. This program demonstrates
 * professional-grade UART handling on Linux, designed in the context of
 * RISC-V M-Mode firmware validation and ACT compliance testing.
 *
 * Features:
 *   - CLI-configurable UART parameters (baud, parity, stop bits, etc.)
 *   - Transmit test messages over UART
 *   - Receive data using poll()-based non-blocking I/O with timeout
 *   - Hex dump display of received data for firmware debugging
 *   - Signal-safe graceful shutdown (Ctrl+C cleanup)
 *   - Comprehensive error handling with actionable messages
 *
 * Usage:
 *   ./uart_tool -d /dev/ttyUSB0 -b 115200 -w "Hello RISC-V"
 *   ./uart_tool --device /dev/ttyS0 --baud 9600 --parity even
 *   ./uart_tool --help
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
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * Program Constants
 * ---------------------------------------------------------------------------
 */
#define PROGRAM_NAME    "uart_tool"
#define PROGRAM_VERSION "1.0.0"

/* Maximum size for the receive buffer (4 KB) */
#define RX_BUFFER_SIZE  4096

/* Default test message — RISC-V themed to show domain awareness */
#define DEFAULT_MESSAGE "RISC-V ACT Framework: UART Communication Test\n"

/*
 * Distinct exit codes for different failure modes.
 * This allows scripts to programmatically determine what went wrong.
 */
#define EXIT_OK         0
#define EXIT_ARGS       1   /* Invalid command-line arguments         */
#define EXIT_OPEN       2   /* Failed to open UART device             */
#define EXIT_WRITE      3   /* Failed to transmit data                */
#define EXIT_READ       4   /* Fatal error during data reception      */

/* ---------------------------------------------------------------------------
 * Signal Handling
 *
 * When running long receive loops (e.g., monitoring boot output from a
 * RISC-V board), the user may press Ctrl+C to stop. Without a signal
 * handler, the program would exit immediately, potentially leaving the
 * serial port in a misconfigured state.
 *
 * Our handler sets a flag that the main loop checks, allowing graceful
 * cleanup (restoring original termios settings, closing the fd).
 *
 * volatile sig_atomic_t is the only type guaranteed safe to modify in
 * a signal handler context (per C11 and POSIX standards).
 * ---------------------------------------------------------------------------
 */
static volatile sig_atomic_t g_running = 1;

static void signal_handler(int signum)
{
    (void)signum;  /* Suppress unused parameter warning */
    g_running = 0;
}

/*
 * install_signal_handlers — Register handlers for clean shutdown signals.
 *
 * We use sigaction() instead of signal() because:
 *   - sigaction() has well-defined, portable behavior (signal() varies)
 *   - SA_RESTART flag controls whether interrupted syscalls auto-restart
 *   - We explicitly don't set SA_RESTART so that poll() will return
 *     with EINTR, allowing the main loop to check g_running promptly
 */
static void install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  /* No SA_RESTART: let poll() be interrupted */

    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/* ---------------------------------------------------------------------------
 * CLI Argument Parsing
 * ---------------------------------------------------------------------------
 */
static void print_usage(void)
{
    printf(
        ANSI_BLUE "╔══════════════════════════════════════════════════════╗\n"
                   "║       RISC-V UART Communication Tool v%s        ║\n"
                   "╚══════════════════════════════════════════════════════╝\n"
        ANSI_RESET "\n"
        "Usage: %s [OPTIONS]\n\n"
        "Options:\n"
        "  -d, --device PATH     Serial device path      (default: /dev/ttyUSB0)\n"
        "  -b, --baud RATE       Baud rate                (default: 115200)\n"
        "  -p, --parity TYPE     Parity: none|even|odd    (default: none)\n"
        "  -s, --stop BITS       Stop bits: 1|2           (default: 1)\n"
        "  -n, --databits BITS   Data bits: 5|6|7|8       (default: 8)\n"
        "  -w, --write MSG       Message to transmit\n"
        "  -t, --timeout MS      Read timeout in ms       (default: 5000)\n"
        "  -v, --verbose         Enable debug output\n"
        "  -h, --help            Show this help message\n"
        "\n"
        "Examples:\n"
        "  %s -d /dev/ttyUSB0 -b 115200 -w \"Hello RISC-V\"\n"
        "  %s -d /dev/ttyS0 -b 9600 -p even -s 2\n"
        "  %s --device /dev/ttyUSB0 --verbose\n"
        "\n"
        "Common RISC-V Board Baud Rates:\n"
        "  115200  — SiFive HiFive, StarFive VisionFive (most common)\n"
        "  9600    — Legacy sensors and peripherals\n"
        "  921600  — High-speed debug trace output\n"
        "\n",
        PROGRAM_VERSION, PROGRAM_NAME,
        PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME
    );
}

/*
 * parse_parity — Convert a CLI string to a uart_parity_t enum value.
 * Returns -1 if the string is not recognized.
 */
static int parse_parity(const char *str, uart_parity_t *out)
{
    if (strcasecmp(str, "none") == 0 || strcmp(str, "n") == 0) {
        *out = UART_PARITY_NONE;
    } else if (strcasecmp(str, "even") == 0 || strcmp(str, "e") == 0) {
        *out = UART_PARITY_EVEN;
    } else if (strcasecmp(str, "odd") == 0 || strcmp(str, "o") == 0) {
        *out = UART_PARITY_ODD;
    } else {
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Main Program
 * ---------------------------------------------------------------------------
 */
int main(int argc, char *argv[])
{
    /* Start with default configuration (115200 8N1) */
    uart_config_t config = uart_config_default();
    const char *tx_message = NULL;

    /*
     * Long options table for getopt_long.
     * Each entry maps: {long_name, has_arg, flag_ptr, short_char}
     */
    static const struct option long_opts[] = {
        {"device",   required_argument, NULL, 'd'},
        {"baud",     required_argument, NULL, 'b'},
        {"parity",   required_argument, NULL, 'p'},
        {"stop",     required_argument, NULL, 's'},
        {"databits", required_argument, NULL, 'n'},
        {"write",    required_argument, NULL, 'w'},
        {"timeout",  required_argument, NULL, 't'},
        {"verbose",  no_argument,       NULL, 'v'},
        {"help",     no_argument,       NULL, 'h'},
        {NULL,       0,                 NULL,  0 }
    };

    /* Parse command-line arguments */
    int opt;
    while ((opt = getopt_long(argc, argv, "d:b:p:s:n:w:t:vh", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'd':
                config.device_path = optarg;
                break;

            case 'b': {
                char *endptr;
                unsigned long baud = strtoul(optarg, &endptr, 10);
                if (*endptr != '\0' || baud == 0) {
                    LOG_ERROR("Invalid baud rate: '%s'", optarg);
                    return EXIT_ARGS;
                }
                config.baud_rate = (unsigned int)baud;
                break;
            }

            case 'p':
                if (parse_parity(optarg, &config.parity) != 0) {
                    LOG_ERROR("Invalid parity: '%s' (use: none, even, odd)", optarg);
                    return EXIT_ARGS;
                }
                break;

            case 's': {
                int stop = atoi(optarg);
                if (stop != 1 && stop != 2) {
                    LOG_ERROR("Invalid stop bits: '%s' (use: 1 or 2)", optarg);
                    return EXIT_ARGS;
                }
                config.stop_bits = stop;
                break;
            }

            case 'n': {
                int dbits = atoi(optarg);
                if (dbits < 5 || dbits > 8) {
                    LOG_ERROR("Invalid data bits: '%s' (use: 5, 6, 7, or 8)", optarg);
                    return EXIT_ARGS;
                }
                config.data_bits = dbits;
                break;
            }

            case 'w':
                tx_message = optarg;
                break;

            case 't': {
                char *endptr;
                long timeout = strtol(optarg, &endptr, 10);
                if (*endptr != '\0' || timeout < 0) {
                    LOG_ERROR("Invalid timeout: '%s' (must be non-negative integer)", optarg);
                    return EXIT_ARGS;
                }
                config.timeout_ms = (int)timeout;
                break;
            }

            case 'v':
                config.verbose = 1;
                break;

            case 'h':
                print_usage();
                return EXIT_OK;

            default:
                print_usage();
                return EXIT_ARGS;
        }
    }

    /* Install signal handlers for graceful shutdown */
    install_signal_handlers();

    /* Print startup banner */
    printf(ANSI_BLUE
           "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
           "  RISC-V UART Communication Tool v%s\n"
           "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
           ANSI_RESET, PROGRAM_VERSION);

    /* -----------------------------------------------------------------------
     * Phase 1: Open and configure the UART device
     *
     * uart_open() handles the full initialization sequence:
     *   1. open() with appropriate flags
     *   2. isatty() verification
     *   3. TIOCEXCL exclusive lock
     *   4. termios configuration
     *   5. Read-back verification
     * ----------------------------------------------------------------------- */
    int fd = uart_open(&config);
    if (fd < 0) {
        LOG_ERROR("UART initialization failed: %s", uart_strerror((uart_error_t)fd));
        return EXIT_OPEN;
    }

    int exit_code = EXIT_OK;

    /* -----------------------------------------------------------------------
     * Phase 2: Transmit data
     *
     * If the user provided a message via -w/--write, send it now.
     * Otherwise, send the default RISC-V themed test message.
     * ----------------------------------------------------------------------- */
    const char *msg = tx_message ? tx_message : DEFAULT_MESSAGE;

    printf(ANSI_YELLOW "\n[TX] Sending message (%zu bytes):\n" ANSI_RESET, strlen(msg));
    printf("  \"%s\"\n", msg);

    ssize_t written = uart_write_data(fd, msg, strlen(msg));
    if (written < 0) {
        LOG_ERROR("Transmission failed: %s", uart_strerror((uart_error_t)written));
        exit_code = EXIT_WRITE;
        goto cleanup;
    }

    printf(ANSI_GREEN "  ✓ %zd bytes transmitted successfully\n" ANSI_RESET, written);

    /* -----------------------------------------------------------------------
     * Phase 3: Receive data
     *
     * Enter a receive loop that continues until:
     *   a) A timeout occurs (no data within the configured window)
     *   b) The user presses Ctrl+C (g_running set to 0 by signal handler)
     *   c) A fatal error occurs (device disconnect, etc.)
     *
     * This loop pattern is representative of how firmware validation tools
     * continuously read UART output from a RISC-V board during testing:
     * the board outputs test results, boot logs, or compliance signatures,
     * and the host tool captures everything until the test completes.
     * ----------------------------------------------------------------------- */
    printf(ANSI_YELLOW "\n[RX] Listening for incoming data "
           "(timeout: %dms, press Ctrl+C to stop)...\n" ANSI_RESET,
           config.timeout_ms);

    char rx_buf[RX_BUFFER_SIZE];
    size_t total_received = 0;
    int consecutive_timeouts = 0;
    const int MAX_TIMEOUTS = 3;  /* Stop after 3 consecutive timeouts */

    while (g_running && consecutive_timeouts < MAX_TIMEOUTS) {
        ssize_t n = uart_read_data(fd, rx_buf, sizeof(rx_buf) - 1, config.timeout_ms);

        if (n > 0) {
            /* Data received — reset timeout counter and display it */
            consecutive_timeouts = 0;
            total_received += (size_t)n;
            rx_buf[n] = '\0';  /* Null-terminate for safe printing */

            printf(ANSI_GREEN "\n[RX] Received %zd bytes:\n" ANSI_RESET, n);

            /* ASCII representation */
            printf("  ASCII: \"%s\"\n", rx_buf);

            /* Hex dump for binary inspection */
            printf("  Hex dump:\n");
            hexdump(rx_buf, (size_t)n);

        } else if (n == UART_ERR_TIMEOUT) {
            consecutive_timeouts++;
            printf(ANSI_YELLOW "  ⏳ Timeout %d/%d (no data received)\n" ANSI_RESET,
                   consecutive_timeouts, MAX_TIMEOUTS);

        } else if (n == 0) {
            /* Interrupted or spurious wake-up — continue if still running */
            continue;

        } else {
            /* Fatal error */
            LOG_ERROR("Read error: %s", uart_strerror((uart_error_t)n));
            exit_code = EXIT_READ;
            break;
        }
    }

    /* Print summary */
    printf(ANSI_BLUE "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
           "  Session Summary\n"
           "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
           ANSI_RESET);
    printf("  Bytes transmitted:  %zd\n", written);
    printf("  Bytes received:     %zu\n", total_received);
    printf("  Exit reason:        %s\n",
           !g_running ? "User interrupt (Ctrl+C)" :
           consecutive_timeouts >= MAX_TIMEOUTS ? "Read timeout" :
           exit_code != EXIT_OK ? "Error" : "Complete");

    /* -----------------------------------------------------------------------
     * Phase 4: Cleanup
     *
     * uart_close() restores original termios settings and closes the fd.
     * The goto target ensures cleanup happens even on error paths.
     * ----------------------------------------------------------------------- */
cleanup:
    uart_close(fd);

    printf(ANSI_BLUE "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
           ANSI_RESET);

    return exit_code;
}
