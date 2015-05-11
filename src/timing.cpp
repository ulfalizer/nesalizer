#include "common.h"

#include "mapper.h"
#include "rom.h"
#include "timing.h"

double cpu_clock_rate;
double ppu_clock_rate;
double ppu_fps;

void init_timing_for_rom() {
    if (is_pal) {
        double master_clock_rate = 26601712.0;
        cpu_clock_rate           = master_clock_rate/16.0; // ~1.66 MHz
        ppu_clock_rate           = master_clock_rate/5.0; // ~5.32 MHz
        ppu_fps                  = ppu_clock_rate/(341*312); // ~50.0 FPS
    }
    else {
        double master_clock_rate = 21477272.0;
        cpu_clock_rate           = master_clock_rate/12.0; // ~1.79 MHz
        ppu_clock_rate           = master_clock_rate/4.0; // ~5.37 MHz
        ppu_fps                  = ppu_clock_rate/(341*261 + 340.5); // ~60.1 FPS
    }
}

// TODO: Use SDL_Delay() instead? Higher sleep precision is good for audio
// buffer management, but that needs to be quantified, and whether
// clock_nanosleep() gives a noticeable precision improvement depends on OS
// scheduling.

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
    add_to_timespec(clock_previous, 1e9/ppu_fps);
again:
    int const res =
      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &clock_previous, 0);
    if (res == EINTR) goto again;
    errno_val_fail_if(res != 0, res, "failed to sleep with clock_nanosleep()");
    errno_fail_if(clock_gettime(CLOCK_MONOTONIC, &clock_previous) == -1,
      "failed to fetch synchronization timestamp from clock_gettime()");
}
