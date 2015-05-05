#include "common.h"

#include <execinfo.h>
#include <signal.h>

#define SAFE_STDERR_PRINT(msg) write(STDERR_FILENO, msg, ARRAY_LEN(msg) - 1)

// Backtrace generation on segfault

// Avoid warnings from unused write() return values. There's nothing useful we
// can do in that case anyway.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
static void sigsegv_handler(int) {
    static void *backtrace_buffer[100];
    static char addr2line_cmd_buf[100];

    static char const segfault_msg[] =
      "--- Segmentation fault! Backtrace: ---\n\n";
    static char const addr2line_msg[] =
      "\n--- Attempting to get line numbers with addr2line: ---\n\n";
    static char const addr2line_open_fail_msg[] = "Failed to run addr2line\n";
    static char const addr2line_write_fail_msg[] =
      "Failed to write to addr2line (after successfully running it)\n";
    static char const addr2line_close_fail_msg[] =
      "Failed to close connection to addr2line (after successfully running and writing to it)\n";
    static char const end_msg[] = "\n--- End of backtrace ---\n";

    SAFE_STDERR_PRINT(segfault_msg);

    int const n = backtrace(backtrace_buffer, ARRAY_LEN(backtrace_buffer));

    // Print a basic backtrace w/o debug information. If automatic addr2line
    // translation fails, we can invoke it manually using the returned
    // addresses to get line numbers.

    backtrace_symbols_fd(backtrace_buffer, n, 2);

    // Try to get more information using addr2line

    SAFE_STDERR_PRINT(addr2line_msg);

    sprintf(addr2line_cmd_buf, "addr2line -Cfip -e '%s'", program_name);
    // Could use lower-level APIs to avoid libc and find the program in a more
    // robust way (e.g. via /proc) too
    FILE *const f = popen(addr2line_cmd_buf, "w");
    if (!f) {
        SAFE_STDERR_PRINT(addr2line_open_fail_msg);
        abort();
    }

    for (int i = 0; i < n; ++i)
        if (fprintf(f, "%p\n", backtrace_buffer[i]) < 0) {
            SAFE_STDERR_PRINT(addr2line_write_fail_msg);
            abort();
        }

    if (pclose(f) == -1) {
        SAFE_STDERR_PRINT(addr2line_close_fail_msg);
        abort();
    }

    SAFE_STDERR_PRINT(end_msg);

    abort();
}
#pragma GCC diagnostic pop

static void install_sigsegv_handler() {
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = sigsegv_handler;
    fail_if(sigaction(SIGSEGV, &sa, 0) == -1,
            "failed to install SIGSEGV handler");
}

void init_debug() {
    install_sigsegv_handler();
}
