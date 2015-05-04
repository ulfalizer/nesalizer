void init_timing();
void init_timing_for_rom();
void sleep_till_end_of_frame();

extern unsigned long cpu_clock_rate;
extern unsigned long ppu_clock_rate;

// Hack to get a C++03 compile-time constant
unsigned const pal_milliframes_per_second = 50007;
