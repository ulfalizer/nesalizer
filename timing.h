unsigned long const ntsc_master_clock_rate = 21477272;
double const ntsc_cpu_clock_rate = ntsc_master_clock_rate/12.0; // ~1.179 MHz
double const ntsc_ppu_clock_rate = ntsc_master_clock_rate/4.0;  // ~5.369 MHz
unsigned long const nanoseconds_per_frame = 1000000000/(ntsc_ppu_clock_rate/(341*261 + 340.5)); // ~16 ms
// Hack to get a C++03 compile-time constant
unsigned const milliframes_per_second = 60099;

void init_timing();
void sleep_till_end_of_frame();
