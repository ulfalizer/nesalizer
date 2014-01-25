void           run();
void           tick();

uint8_t        read(uint16_t addr);

void           set_nmi(bool s);

void           set_cart_irq(bool s);
void           set_dmc_irq(bool s);
void           set_frame_irq(bool s);

void           frame_completed();
void           soft_reset();
void           end_emulation();

// TODO: Might want to move these to apu.cpp (along with moving cart_irq)
extern bool    dmc_irq;
extern bool    frame_irq;

extern bool    cpu_is_reading;

extern uint8_t cpu_data_bus;

template<bool calculating_size, bool is_save>
void transfer_cpu_state(uint8_t *&buf);
