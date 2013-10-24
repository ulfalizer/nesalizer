#include "common.h"

#include "timing.h"

#include <time.h>

// SDL's timing functions only have millisecond precision, which doesn't seem
// good enough (60 FPS is approx. 16 ms per frame). Roll our own.

// Used for main loop synchronization
static timespec clock_previous;

static void add_to_timespec(timespec &ts, long nano_secs) {
    assert(nano_secs <= 1000000000l);
    long const new_nanos = ts.tv_nsec + nano_secs;
    ts.tv_sec += (new_nanos >= 1000000000l);
    ts.tv_nsec = new_nanos%1000000000l;
}

void init_timing() {
    errno_fail_if(clock_gettime(CLOCK_MONOTONIC, &clock_previous) < 0,
      "failed to fetch initial synchronization timestamp from clock_gettime()");
}

void sleep_till_end_of_frame() {
    add_to_timespec(clock_previous, nanoseconds_per_frame);
again:
    int const res =
      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &clock_previous, 0);
    if (res == EINTR) goto again;
    errno_val_fail_if(res != 0, res, "failed to sleep with clock_nanosleep()");
    errno_fail_if(clock_gettime(CLOCK_MONOTONIC, &clock_previous) < 0,
      "failed to fetch synchronization timestamp from clock_gettime()");
}
