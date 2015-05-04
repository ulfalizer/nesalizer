#include "common.h"

#include "rom.h"
#include "timing.h"

#include <time.h>

unsigned long const ntsc_master_clock_rate = 21477272;
double const        ntsc_cpu_clock_rate    = ntsc_master_clock_rate/12.0; // ~1.179 MHz
double const        ntsc_ppu_clock_rate    = ntsc_master_clock_rate/4.0;  // ~5.369 MHz
unsigned long const ntsc_nanos_per_frame   = 1000000000/(ntsc_ppu_clock_rate/(341*261 + 340.5)); // ~16 ms

unsigned long const pal_master_clock_rate  = 26601712;
double const        pal_cpu_clock_rate     = pal_master_clock_rate/16.0; // ~1.663 MHz
double const        pal_ppu_clock_rate     = pal_master_clock_rate/5.0;  // ~5.320 MHz
unsigned long const pal_nanos_per_frame    = 1000000000/(pal_ppu_clock_rate/(341*312)); // ~20 ms

unsigned long cpu_clock_rate;
unsigned long ppu_clock_rate;
static unsigned long nanos_per_frame;

void init_timing_for_rom() {
    if (is_pal) {
        cpu_clock_rate  = pal_cpu_clock_rate;
        ppu_clock_rate  = pal_ppu_clock_rate;
        nanos_per_frame = pal_nanos_per_frame;
    }
    else {
        cpu_clock_rate  = ntsc_cpu_clock_rate;
        ppu_clock_rate  = ntsc_ppu_clock_rate;
        nanos_per_frame = ntsc_nanos_per_frame;
    }
}

// SDL's timing functions only have millisecond precision, which doesn't seem
// good enough (60 FPS is approx. 16 ms per frame). Roll our own.

// Used for main loop synchronization
static timespec clock_previous;

static void add_to_timespec(timespec &ts, long nano_secs) {
    long const new_nanos = ts.tv_nsec + nano_secs;
    ts.tv_sec += new_nanos/1000000000l;
    ts.tv_nsec = new_nanos%1000000000l;
}

void init_timing() {
    errno_fail_if(clock_gettime(CLOCK_MONOTONIC, &clock_previous) == -1,
      "failed to fetch initial synchronization timestamp from clock_gettime()");
}

void sleep_till_end_of_frame() {
    add_to_timespec(clock_previous, nanos_per_frame);
again:
    int const res =
      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &clock_previous, 0);
    if (res == EINTR) goto again;
    errno_val_fail_if(res != 0, res, "failed to sleep with clock_nanosleep()");
    errno_fail_if(clock_gettime(CLOCK_MONOTONIC, &clock_previous) == -1,
      "failed to fetch synchronization timestamp from clock_gettime()");
}
