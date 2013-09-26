void init_cpu();

// Sets the components to their cold boot state and starts emulation
void run();

void tick();

extern bool nmi_asserted;
extern bool cart_irq, dmc_irq, frame_irq;
void update_irq_status();

extern uint8_t cpu_data_bus;

extern bool cpu_is_reading;
extern enum OAM_DMA_state {
    OAM_DMA_IN_PROGRESS = 0,
    OAM_DMA_IN_PROGRESS_3RD_TO_LAST_TICK,
    OAM_DMA_IN_PROGRESS_LAST_TICK,
    OAM_DMA_NOT_IN_PROGRESS
} oam_dma_state;

// Set true to break out of the emulation loop
extern bool end_emulation;
