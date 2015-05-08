// 6502/2A03/2A07 core.
//
// http://wiki.nesdev.com/w/index.php/CPU

// Current CPU read/write state. Needed to get the timing for APU DMC sample
// loading right (tested by the sprdma_and_dmc_dma tests).
extern bool cpu_is_reading;

// Last value put on the CPU data bus. Used to implement open bus reads.
extern uint8_t cpu_data_bus;

// Offset in CPU cycles within the current frame. Used for audio generation.
extern unsigned frame_offset;

// Runs the PPU and APU for one CPU cycle. Has external linkage so we can use
// it while the CPU is halted during DMA.
void tick();

// Also used outside the CPU core to load DMC samples - hence the external
// linkage
uint8_t read_mem(uint16_t addr);

// Interrupt source status
void set_nmi(bool s);
void set_cart_irq(bool s);
void set_dmc_irq(bool s);
void set_frame_irq(bool s);

// Starts emulation by issuing a RESET interrupt and entering the emulation
// loop
void run();

// These functions inform the CPU emulation code of various events, which are
// handled at the next instruction boundary. Handling events at instruction
// boundaries simplifies state transfers as the current location within the CPU
// emulation loop is part of the state.

// Signaled at the end of the visible portion of the frame
void frame_completed();
// Signaled if the reset button was pushed
void soft_reset();
// Signaled if emulation should end
void end_emulation();

template<bool calculating_size, bool is_save>
void transfer_cpu_state(uint8_t *&buf);
