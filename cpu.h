void init_cpu();

// Sets the components to their cold boot state and starts emulation
void run();

void tick();
uint8_t read(uint16_t addr);
void write(uint8_t value, uint16_t addr);

extern bool nmi_asserted;
extern bool cart_irq, dmc_irq, frame_irq;
void update_irq_status();

extern uint8_t cpu_data_bus;

extern bool cpu_is_reading;

// Set true to break out of the emulation loop
extern bool end_emulation;
