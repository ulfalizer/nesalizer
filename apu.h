extern bool apu_clk1_is_high;

void init_apu();
void set_apu_cold_boot_state();
void reset_apu();

void write_pulse_reg_0(unsigned n, uint8_t value);
void write_pulse_reg_1(unsigned n, uint8_t value);
void write_pulse_reg_2(unsigned n, uint8_t value);
void write_pulse_reg_3(unsigned n, uint8_t value);

void write_triangle_reg_0(uint8_t value);
void write_triangle_reg_1(uint8_t value);
void write_triangle_reg_2(uint8_t value);

void write_noise_reg_0(uint8_t value);
void write_noise_reg_1(uint8_t value);
void write_noise_reg_2(uint8_t value);

void write_dmc_reg_0(uint8_t value);
void write_dmc_reg_1(uint8_t value);
void write_dmc_reg_2(uint8_t value);
void write_dmc_reg_3(uint8_t value);

uint8_t read_apu_status();
void write_apu_status(uint8_t value);

void write_frame_counter(uint8_t value);

void do_oam_dma(uint8_t addr);

void tick_apu();

template<bool calculating_size, bool is_save>
void transfer_apu_state(uint8_t *&buf);
