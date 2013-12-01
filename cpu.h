void           init_cpu();
void           run();
void           tick();

void           update_irq_status();

uint8_t        read(uint16_t addr);

extern bool    end_emulation;
extern bool    pending_event;
extern bool    pending_reset;

extern bool    nmi_asserted;
extern bool    cart_irq, dmc_irq, frame_irq;

extern bool    cpu_is_reading;

extern uint8_t cpu_data_bus;

template<bool calculating_size, bool is_save>
void transfer_cpu_state(uint8_t *&buf);
