#include "common.h"

#include <execinfo.h>
#include <signal.h>

//
// General utility functions
//

bool is_pow_2_or_0(unsigned n) {
    return !(n & (n - 1));
}

uint8_t rev_byte(uint8_t n) {
    static uint8_t const rev_table[] = {
      0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0, 0x10, 0x90, 0x50, 0xD0, 0x30, 0xB0, 0x70, 0xF0,
      0x08, 0x88, 0x48, 0xC8, 0x28, 0xA8, 0x68, 0xE8, 0x18, 0x98, 0x58, 0xD8, 0x38, 0xB8, 0x78, 0xF8,
      0x04, 0x84, 0x44, 0xC4, 0x24, 0xA4, 0x64, 0xE4, 0x14, 0x94, 0x54, 0xD4, 0x34, 0xB4, 0x74, 0xF4,
      0x0C, 0x8C, 0x4C, 0xCC, 0x2C, 0xAC, 0x6C, 0xEC, 0x1C, 0x9C, 0x5C, 0xDC, 0x3C, 0xBC, 0x7C, 0xFC,
      0x02, 0x82, 0x42, 0xC2, 0x22, 0xA2, 0x62, 0xE2, 0x12, 0x92, 0x52, 0xD2, 0x32, 0xB2, 0x72, 0xF2,
      0x0A, 0x8A, 0x4A, 0xCA, 0x2A, 0xAA, 0x6A, 0xEA, 0x1A, 0x9A, 0x5A, 0xDA, 0x3A, 0xBA, 0x7A, 0xFA,
      0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6, 0x16, 0x96, 0x56, 0xD6, 0x36, 0xB6, 0x76, 0xF6,
      0x0E, 0x8E, 0x4E, 0xCE, 0x2E, 0xAE, 0x6E, 0xEE, 0x1E, 0x9E, 0x5E, 0xDE, 0x3E, 0xBE, 0x7E, 0xFE,
      0x01, 0x81, 0x41, 0xC1, 0x21, 0xA1, 0x61, 0xE1, 0x11, 0x91, 0x51, 0xD1, 0x31, 0xB1, 0x71, 0xF1,
      0x09, 0x89, 0x49, 0xC9, 0x29, 0xA9, 0x69, 0xE9, 0x19, 0x99, 0x59, 0xD9, 0x39, 0xB9, 0x79, 0xF9,
      0x05, 0x85, 0x45, 0xC5, 0x25, 0xA5, 0x65, 0xE5, 0x15, 0x95, 0x55, 0xD5, 0x35, 0xB5, 0x75, 0xF5,
      0x0D, 0x8D, 0x4D, 0xCD, 0x2D, 0xAD, 0x6D, 0xED, 0x1D, 0x9D, 0x5D, 0xDD, 0x3D, 0xBD, 0x7D, 0xFD,
      0x03, 0x83, 0x43, 0xC3, 0x23, 0xA3, 0x63, 0xE3, 0x13, 0x93, 0x53, 0xD3, 0x33, 0xB3, 0x73, 0xF3,
      0x0B, 0x8B, 0x4B, 0xCB, 0x2B, 0xAB, 0x6B, 0xEB, 0x1B, 0x9B, 0x5B, 0xDB, 0x3B, 0xBB, 0x7B, 0xFB,
      0x07, 0x87, 0x47, 0xC7, 0x27, 0xA7, 0x67, 0xE7, 0x17, 0x97, 0x57, 0xD7, 0x37, 0xB7, 0x77, 0xF7,
      0x0F, 0x8F, 0x4F, 0xCF, 0x2F, 0xAF, 0x6F, 0xEF, 0x1F, 0x9F, 0x5F, 0xDF, 0x3F, 0xBF, 0x7F, 0xFF };
    return rev_table[n];
}

uint8_t *get_file_buffer(char const *filename, size_t &size_out) {
    FILE *file;
    uint8_t *file_buf;
    long file_size;

    // We do not support large files on 32-bit systems

    errno_fail_if(!(file = fopen(filename, "rb")), "failed to open '%s'", filename);
    errno_fail_if(fseek(file, 0, SEEK_END) == -1, "failed to seek to end of '%s'", filename);
    errno_fail_if((file_size = ftell(file)) == -1, "failed to get size of '%s'", filename);
    errno_fail_if(fseek(file, 0, SEEK_SET) == -1, "failed to seek back to beginning of '%s'", filename);

    fail_if(!(file_buf = new (std::nothrow) unsigned char[file_size]),
            "failed to allocate %ld-byte buffer for '%s'", file_size, filename);
    size_t const fread_res = fread(file_buf, 1, file_size, file);
    if ((unsigned long long)fread_res < (unsigned long long)file_size) {
        fail_if(feof(file), "unexpected end of file while reading '%s'", filename);
        fail_if(ferror(file), "I/O error while reading '%s'", filename);
        fail("unknown error while reading '%s'", filename);
    }
    errno_fail_if(fclose(file) == EOF, "failed to close '%s'", filename);

    size_out = file_size;
    return file_buf;
}

//
// Error reporting
//

static void fail_helper(bool include_errno, char const *format, va_list args)
  __attribute__((format(printf, 2, 0), noreturn));

static void fail_helper(bool include_errno, char const *format, va_list args) {
    fprintf(stderr, "%s: ", program_name);
    vfprintf(stderr, format, args);
    if (include_errno) {
        char const *const err_str = strerror(errno);
        fprintf(stderr, ": %s (errno = %d)", err_str ? err_str : "unknown error", errno);
    }
    putc('\n', stderr);
    exit(EXIT_FAILURE);
}

void fail(char const *format, ...) {
    va_list args;
    va_start(args, format);
    fail_helper(false, format, args);
}

void errno_fail(int errno_val, char const *format, ...) {
    va_list args;
    va_start(args, format);
    errno = errno_val;
    fail_helper(true, format, args);
}

//
// Backtrace printing on receiving fatal signals
//

static void fatal_signal_handler(int sig) {
    // Use non-async-signal-safe functions. Likely to work in practice.

    static void *backtrace_buffer[100];
    static char addr2line_cmd_buf[100];

    fprintf(stderr, "caught fatal signal '%s'. Backtrace:\n\n", strsignal(sig));

    int const n = backtrace(backtrace_buffer, ARRAY_LEN(backtrace_buffer));

    // Print a basic backtrace w/o debug information. If automatic addr2line
    // translation fails, we can invoke it manually using the returned
    // addresses to get line numbers.

    backtrace_symbols_fd(backtrace_buffer, n, 2);

    fputs("\nattempting to get line numbers with addr2line:\n\n", stderr);

    sprintf(addr2line_cmd_buf, "addr2line -Cfip -e '%s'", program_name);
    FILE *const f = popen(addr2line_cmd_buf, "w");
    if (!f) {
        fputs("failed to run addr2line\n", stderr);
        abort();
    }

    for (int i = 0; i < n; ++i)
        if (fprintf(f, "%p\n", backtrace_buffer[i]) < 0) {
            fputs("failed to write address to addr2line\n", stderr);
            abort();
        }

    abort();
}

static void install_fatal_signal_handler(int sig) {
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = fatal_signal_handler;
    fail_if(sigaction(sig, &sa, 0) == -1,
            "failed to install handler for signal '%s'", strsignal(sig));
}

void install_fatal_signal_handlers() {
    install_fatal_signal_handler(SIGBUS);
    install_fatal_signal_handler(SIGILL);
    install_fatal_signal_handler(SIGSEGV);
}
