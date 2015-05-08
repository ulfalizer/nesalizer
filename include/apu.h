// APU (audio circuitry) implementation
//
// http://wiki.nesdev.com/w/index.php/APU

// Emulates a DMA transfer of sprite data to the PPU
void do_oam_dma(uint8_t addr);

// Invalidates the cached signal level as outlined in set_audio_signal_level()
void begin_audio_frame();

// 'n' is 0 for the first pulse channel and 1 for the second.

void write_pulse_reg_0(unsigned n, uint8_t val); // $4000/$4004
void write_pulse_reg_1(unsigned n, uint8_t val); // $4001/$4005
void write_pulse_reg_2(unsigned n, uint8_t val); // $4002/$4006
void write_pulse_reg_3(unsigned n, uint8_t val); // $4003/$4007

void write_triangle_reg_0(uint8_t val); // $4008
void write_triangle_reg_1(uint8_t val); // $400A
void write_triangle_reg_2(uint8_t val); // $400B

void write_noise_reg_0(uint8_t val); // $400C
void write_noise_reg_1(uint8_t val); // $400E
void write_noise_reg_2(uint8_t val); // $400F

void write_dmc_reg_0(uint8_t val); // $4010
void write_dmc_reg_1(uint8_t val); // $4011
void write_dmc_reg_2(uint8_t val); // $4012
void write_dmc_reg_3(uint8_t val); // $4013
// IRQ line from DMC
extern bool dmc_irq;

void write_frame_counter(uint8_t val); // $4017
// IRQ line from frame counter
extern bool frame_irq;

// $4015
uint8_t read_apu_status();
void write_apu_status(uint8_t val);

void init_apu();
void init_apu_for_rom();

void reset_apu();
void set_apu_cold_boot_state();

void tick_apu();

template<bool calculating_size, bool is_save>
void transfer_apu_state(uint8_t *&buf);
