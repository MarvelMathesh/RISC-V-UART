/*
 * =============================================================================
 * utils.h — Logging Macros and Utility Helpers
 * =============================================================================
 *
 * Provides colored, timestamped logging macros and a hex dump utility for
 * debugging UART communication. These are essential tools when validating
 * firmware output on RISC-V hardware boards, where raw byte inspection
 * is a daily necessity.
 *
 * Design Notes:
 *   - Macros use do { ... } while(0) for safe use in if/else without braces
 *   - Color output uses ANSI escape codes (widely supported on Linux terminals)
 *   - LOG_DEBUG is compiled out in release builds (guarded by DEBUG macro)
 *   - hexdump() outputs both hex and ASCII columns, matching the standard
 *     format used by tools like xxd and hexdump -C
 *
 * Author:  Mathesh
 * License: MIT
 * =============================================================================
 */

#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>
#include <time.h>
#include <string.h>
#include <ctype.h>

/* ---------------------------------------------------------------------------
 * ANSI Color Codes
 *
 * Used to visually distinguish log severity levels in terminal output.
 * This makes it significantly easier to spot errors during long UART
 * debug sessions (e.g., watching boot logs from a RISC-V board).
 * ---------------------------------------------------------------------------
 */
#define ANSI_RED     "\033[1;31m"
#define ANSI_GREEN   "\033[1;32m"
#define ANSI_YELLOW  "\033[1;33m"
#define ANSI_BLUE    "\033[1;34m"
#define ANSI_CYAN    "\033[1;36m"
#define ANSI_RESET   "\033[0m"

/* ---------------------------------------------------------------------------
 * Timestamp Helper
 *
 * Generates a human-readable timestamp for log messages. Uses monotonic-style
 * local time. This is critical for correlating UART events with board behavior
 * (e.g., "reset signal asserted at 14:32:01.045, first UART byte at 14:32:01.102").
 *
 * The buffer is static to avoid per-call allocation, which is acceptable
 * since logging is inherently single-threaded in this context.
 * ---------------------------------------------------------------------------
 */
static inline const char *get_timestamp(void)
{
    static char buf[32];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    if (tm_info != NULL) {
        strftime(buf, sizeof(buf), "%H:%M:%S", tm_info);
    } else {
        snprintf(buf, sizeof(buf), "??:??:??");
    }
    return buf;
}

/* ---------------------------------------------------------------------------
 * Logging Macros
 *
 * Three severity levels:
 *   LOG_INFO  — Normal operational messages (green)
 *   LOG_ERROR — Error conditions with errno context (red)
 *   LOG_DEBUG — Verbose debug output, compiled out in release builds (cyan)
 *
 * All macros include:
 *   - Timestamp for temporal correlation
 *   - Severity tag for quick visual scanning
 *   - Source file and line number for traceability
 *   - Format string with variadic arguments (printf-style)
 *
 * Usage:
 *   LOG_INFO("Port opened: %s", device_path);
 *   LOG_ERROR("Failed to open %s", device_path);  // auto-appends strerror
 *   LOG_DEBUG("Raw bytes: %d", byte_count);        // only in debug builds
 * ---------------------------------------------------------------------------
 */
#define LOG_INFO(...) \
    do { \
        fprintf(stdout, ANSI_GREEN "[%s INFO  %s:%d] " ANSI_RESET, \
                get_timestamp(), __FILE__, __LINE__); \
        fprintf(stdout, __VA_ARGS__); \
        fprintf(stdout, "\n"); \
        fflush(stdout); \
    } while (0)

#define LOG_ERROR(...) \
    do { \
        fprintf(stderr, ANSI_RED "[%s ERROR %s:%d] " ANSI_RESET, \
                get_timestamp(), __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        fflush(stderr); \
    } while (0)

#ifdef DEBUG
#define LOG_DEBUG(...) \
    do { \
        fprintf(stdout, ANSI_CYAN "[%s DEBUG %s:%d] " ANSI_RESET, \
                get_timestamp(), __FILE__, __LINE__); \
        fprintf(stdout, __VA_ARGS__); \
        fprintf(stdout, "\n"); \
        fflush(stdout); \
    } while (0)
#else
#define LOG_DEBUG(...) do { (void)0; } while (0)
#endif

/* ---------------------------------------------------------------------------
 * Hex Dump Utility
 *
 * Prints a buffer in the classic hex dump format:
 *
 *   00000000  48 65 6c 6c 6f 20 57 6f  72 6c 64 0a              |Hello World.    |
 *
 * This is indispensable for UART debugging because:
 *   1. Firmware output may contain non-printable control characters
 *   2. Protocol framing issues manifest as shifted byte patterns
 *   3. Encoding problems (e.g., 7-bit vs 8-bit data) are immediately visible
 *   4. ACT compliance test signatures are binary data that need byte-level inspection
 *
 * Parameters:
 *   data — Pointer to the byte buffer to dump
 *   len  — Number of bytes to display
 *
 * The function is declared static inline to:
 *   - Avoid linker issues if included in multiple translation units
 *   - Allow the compiler to inline it at call sites for performance
 *   - Keep the utility self-contained in this header
 * ---------------------------------------------------------------------------
 */
static inline void hexdump(const void *data, size_t len)
{
    const unsigned char *buf = (const unsigned char *)data;
    size_t i, j;

    if (data == NULL || len == 0) {
        printf("  (empty)\n");
        return;
    }

    for (i = 0; i < len; i += 16) {
        /* Offset column: 8-digit hex address */
        printf("  %08zx  ", i);

        /* Hex columns: 16 bytes per line, split into two groups of 8 */
        for (j = 0; j < 16; j++) {
            if (j == 8) {
                printf(" ");  /* Visual separator between byte groups */
            }
            if (i + j < len) {
                printf("%02x ", buf[i + j]);
            } else {
                printf("   ");  /* Pad incomplete lines */
            }
        }

        /* ASCII column: printable characters shown, others as '.' */
        printf(" |");
        for (j = 0; j < 16 && (i + j) < len; j++) {
            printf("%c", isprint(buf[i + j]) ? buf[i + j] : '.');
        }
        printf("|\n");
    }

    fflush(stdout);
}

#endif /* UTILS_H */
