extern double cpu_clock_rate;
extern double ppu_clock_rate;
extern double ppu_fps;

void init_timing();
void init_timing_for_rom();

// Sleeps until the end of the frame if we manage to emulate it faster than
// realtime (which should hopefully be the case)
void sleep_till_end_of_frame();

// Hack to get a C++03 compile-time constant
unsigned const pal_milliframes_per_second = 50007;
