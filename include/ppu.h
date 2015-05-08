// PPU (graphics processor) emulation. Follows the model in
// http://wiki.nesdev.com/w/images/d/d1/Ntsc_timing.png.
//
// Relevant pages:
//   http://wiki.nesdev.com/w/index.php/PPU_registers
//   http://wiki.nesdev.com/w/index.php/PPU_rendering
//   http://wiki.nesdev.com/w/index.php/The_skinny_on_NES_scrolling

// Nametable memory of variable size, initialized when loading the ROM. 2 KB is
// built in, and the cart can provide an extra 2 KB (though this is rare).
extern uint8_t *ciram;

// The number of the last line in the frame, at the end of the VBlank interval.
// Differs between PAL and NTSC.
extern unsigned prerender_line;

// Optimization - always equals show_bg || show_sprites
extern bool rendering_enabled;

// PPU cycles run so far. Used as a general-purpose timestamp.
extern uint64_t ppu_cycle;

// Current position within the frame
extern unsigned dot, scanline;

// VRAM address currently being output. Some mappers (e.g., MMC3) snoop on
// this.
extern unsigned ppu_addr_bus;

void init_ppu_for_rom();

// These use different timings corresponding to the TV standard
void tick_ntsc_ppu();
void tick_pal_ppu();

// n = 0...7 corresponds to $2000-$2007
uint8_t read_ppu_reg(unsigned n);
void write_ppu_reg(uint8_t val, unsigned n);

void write_oam_data_reg(uint8_t val); // $2004

void set_ppu_cold_boot_state();
void reset_ppu();

template<bool calculating_size, bool is_save>
void transfer_ppu_state(uint8_t *&buf);
