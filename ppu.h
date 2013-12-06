void    init_ppu_for_rom();

void    tick_ntsc_ppu();
void    tick_pal_ppu();

void    set_ppu_cold_boot_state();
void    reset_ppu();
uint8_t read_ppu_reg(unsigned n);
void    write_ppu_reg(uint8_t value, unsigned n);
void    write_oam_data_reg(uint8_t value);

enum Sprite_size { EIGHT_BY_EIGHT = 0, EIGHT_BY_SIXTEEN };

extern bool     pending_frame_completion;
extern unsigned prerender_line;
extern unsigned scanline, dot;
extern unsigned ppu_addr_bus;
extern uint8_t *ciram;
extern uint64_t ppu_cycle;
extern bool     rendering_enabled;

template<bool calculating_size, bool is_save>
void transfer_ppu_state(uint8_t *&buf);
