#include "common.h"

#include "cpu.h"
#include "input.h"
#include "ppu.h"
#include "mapper.h"
#include "rom.h"
#include "sdl_backend.h"
#include "timing.h"

// Set true at the end of the visible part of the frame to trigger end-of-frame
// operations at the next instruction boundary. This simplifies state transfers
// as the current location within the CPU emulation loop is part of the state.
bool           pending_frame_completion;

// If true, treat the emulated code as the first code that runs (i.e., not the
// situation on PowerPak), which means writes to certain registers will be
// inhibited during the initial frame. This breaks some demos.
bool const     starts_on_initial_frame = false;

// NTSC to RGB color conversion. This is influenced by the color tint bits.

uint32_t const nes_to_rgb[8][64] =
    // No emphasis
  { { 0x656565, 0x032863, 0x1a1978, 0x320c75, 0x47065d, 0x520835, 0x511109, 0x431f00,
        0x2d2e00, 0x143a00, 0x004000, 0x003f11, 0x00363e, 0x000000, 0x000000, 0x000000,
      0xaeaeae, 0x2c5cac, 0x4a48c7, 0x6b38c4, 0x8630a3, 0x95336f, 0x933f33, 0x815102,
        0x646500, 0x437500, 0x277d0a, 0x187b3f, 0x1a6f7a, 0x000000, 0x000000, 0x000000,
      0xffffff, 0x7bacfc, 0x9998ff, 0xba87ff, 0xd67ff4, 0xe582be, 0xe38e82, 0xd1a050,
        0xb3b535, 0x92c538, 0x76cd59, 0x67cb8e, 0x69bfca, 0x4e4e4e, 0x000000, 0x000000,
      0xffffff, 0xc8dcfe, 0xd5d4ff, 0xe2cdff, 0xeecafa, 0xf4cbe4, 0xf3d0cb, 0xecd8b7,
        0xdfe0ab, 0xd2e7ad, 0xc6eaba, 0xc0e9d0, 0xc1e4e9, 0xb6b6b6, 0x000000, 0x000000 },

      // Emphasize red
    { 0x5e4643, 0x000d3f, 0x130253, 0x2c0054, 0x400042, 0x4d0022, 0x4f0700, 0x401100,
        0x2a1b00, 0x112300, 0x002500, 0x002100, 0x00161b, 0x000000, 0x000000, 0x000000,
      0xa4837e, 0x23377a, 0x412995, 0x611f96, 0x7d1c7d, 0x8e2253, 0x903023, 0x7c3c00,
        0x5f4a00, 0x3e5400, 0x225800, 0x12521b, 0x10444b, 0x000000, 0x000000, 0x000000,
      0xf1c6c1, 0x6f7abc, 0x8d6cd7, 0xae61d8, 0xca5ec0, 0xdb6495, 0xdd7264, 0xc97f34,
        0xab8d19, 0x8a9718, 0x6e9b31, 0x5d955b, 0x5b868c, 0x47322f, 0x000000, 0x000000,
      0xf1c6c1, 0xbba7bf, 0xc8a1ca, 0xd59dca, 0xe19bc0, 0xe89eaf, 0xe9a49b, 0xe0a987,
        0xd4af7c, 0xc7b37b, 0xbbb485, 0xb4b297, 0xb3acab, 0xab8985, 0x000000, 0x000000 },

      // Emphasize green
    { 0x3f5737, 0x001e3d, 0x000e4c, 0x120147, 0x21002f, 0x2e000f, 0x2f0800, 0x261700,
        0x162700, 0x033400, 0x003c00, 0x003800, 0x002d1f, 0x000000, 0x000000, 0x000000,
      0x7a9b6e, 0x0f4f76, 0x253a8a, 0x3e2884, 0x521d64, 0x632339, 0x65310a, 0x594500,
        0x425a00, 0x296c00, 0x157700, 0x05711f, 0x03634f, 0x000000, 0x000000, 0x000000,
      0xbae6ac, 0x4e99b4, 0x6483c8, 0x7e71c1, 0x9267a1, 0xa36c76, 0xa57b46, 0x998f1d,
        0x83a409, 0x69b610, 0x55c130, 0x44bb5b, 0x42ad8b, 0x2c4225, 0x000000, 0x000000,
      0xbae6ac, 0x8ec6af, 0x97bdb7, 0xa1b6b5, 0xaab1a7, 0xb1b496, 0xb2ba82, 0xadc271,
        0xa3cb69, 0x99d26c, 0x90d779, 0x8ad48a, 0x89ce9e, 0x80a274, 0x000000, 0x000000 },

      // Emphasize red+green
    { 0x3e3e2c, 0x000930, 0x00003f, 0x11003e, 0x21002c, 0x2d000c, 0x2f0000, 0x250a00,
        0x141500, 0x021e00, 0x002300, 0x001e00, 0x001414, 0x000000, 0x000000, 0x000000,
      0x787860, 0x0e3265, 0x242379, 0x3d1777, 0x52115f, 0x621734, 0x642505, 0x573300,
        0x414200, 0x284e00, 0x145400, 0x034e10, 0x014040, 0x000000, 0x000000, 0x000000,
      0xb8b999, 0x4d729e, 0x6463b2, 0x7c56b0, 0x915098, 0xa2566d, 0xa4653c, 0x967317,
        0x808202, 0x678e05, 0x52941d, 0x428e48, 0x408079, 0x2b2c1c, 0x000000, 0x000000,
      0xb8b999, 0x8c9b9b, 0x9595a3, 0xa090a2, 0xa88e98, 0xaf9087, 0xb09673, 0xaa9c63,
        0xa1a25b, 0x97a75c, 0x8eaa66, 0x87a778, 0x87a18c, 0x7e7f65, 0x000000, 0x000000 },

      // Emphasize blue
    { 0x49496c, 0x001a65, 0x110f7a, 0x240275, 0x33005d, 0x3a0038, 0x36000e, 0x270700,
        0x111200, 0x001f00, 0x002700, 0x002818, 0x002342, 0x000000, 0x000003, 0x000003,
      0x8787b7, 0x1f49af, 0x3d3bca, 0x5629c4, 0x6a1ea4, 0x731d72, 0x6e233b, 0x5a300c,
        0x3d3e00, 0x244f00, 0x0f5a17, 0x065c48, 0x0c5580, 0x000003, 0x000003, 0x000003,
      0xcccbff, 0x638cff, 0x807eff, 0x9a6cff, 0xae62f7, 0xb860c5, 0xb2678c, 0x9e735d,
        0x818141, 0x679348, 0x539e68, 0x49a09a, 0x4f99d2, 0x353554, 0x000003, 0x000003,
      0xcccbff, 0xa1b1ff, 0xadacff, 0xb7a4ff, 0xc0a0ff, 0xc49fee, 0xc1a2d7, 0xb9a7c3,
        0xadadb8, 0xa2b4bb, 0x9ab9c8, 0x96b9dd, 0x98b7f4, 0x8e8dc0, 0x000003, 0x000003 },

      // Emphasize blue+red
    { 0x433747, 0x000840, 0x0b0055, 0x1d0053, 0x2d0041, 0x350023, 0x340002, 0x250200,
        0x0f0c00, 0x001500, 0x001900, 0x001800, 0x00111d, 0x000000, 0x000000, 0x000000,
      0x7f6e84, 0x17307c, 0x342297, 0x4d1695, 0x61107d, 0x6c1255, 0x6b1b29, 0x572800,
        0x3a3500, 0x214100, 0x0d4700, 0x024620, 0x033d4d, 0x000000, 0x000000, 0x000000,
      0xc1abc8, 0x586dc0, 0x765fdb, 0x8e52d9, 0xa34cc0, 0xae4e99, 0xad576c, 0x99643c,
        0x7b7221, 0x627e23, 0x4e843b, 0x428263, 0x447990, 0x302533, 0x000000, 0x000000,
      0xc1abc8, 0x9692c5, 0xa28cd0, 0xac87cf, 0xb584c5, 0xb985b5, 0xb989a2, 0xb18e8f,
        0xa49483, 0x9a9984, 0x929b8e, 0x8d9a9f, 0x8e97b1, 0x85748b, 0x000000, 0x000000 },

      // Emphasize blue+green
    { 0x344041, 0x001240, 0x00064f, 0x0e004a, 0x1e0032, 0x260015, 0x250000, 0x1b0300,
        0x0a0f00, 0x001c00, 0x002400, 0x002303, 0x001c24, 0x000000, 0x000000, 0x000000,
      0x6a7a7c, 0x093d7b, 0x1f2f8f, 0x381d88, 0x4d1269, 0x581441, 0x561d15, 0x492b00,
        0x333900, 0x1a4b00, 0x055602, 0x00542a, 0x004b56, 0x000000, 0x000000, 0x000000,
      0xa6bbbe, 0x447ebc, 0x5a6ed1, 0x745cca, 0x8852aa, 0x935382, 0x925c55, 0x846a30,
        0x6e7a1c, 0x558c22, 0x409642, 0x35956a, 0x368c97, 0x222d2e, 0x000000, 0x000000,
      0xa6bbbe, 0x7ea2bd, 0x879cc6, 0x9194c3, 0x9a90b6, 0x9e90a5, 0x9e9493, 0x989a83,
        0x8fa07b, 0x84a87e, 0x7cac8b, 0x77ab9b, 0x78a8ae, 0x708183, 0x000000, 0x000000 },

      // Emphasize all
    { 0x343434, 0x000633, 0x000042, 0x0e0040, 0x1e002e, 0x260010, 0x250000, 0x1b0000,
        0x0a0b00, 0x001400, 0x001900, 0x001700, 0x001117, 0x000000, 0x000000, 0x000000,
      0x6a6a6a, 0x0a2d69, 0x201f7d, 0x38137a, 0x4d0d62, 0x580e3b, 0x56170f, 0x492500,
        0x333400, 0x1a4000, 0x064600, 0x004417, 0x003b44, 0x000000, 0x000000, 0x000000,
      0xa6a6a6, 0x4569a5, 0x5b5ab9, 0x744db7, 0x88479e, 0x944977, 0x92524a, 0x846024,
        0x6e6f10, 0x557c12, 0x41822b, 0x358052, 0x37777f, 0x232323, 0x000000, 0x000000,
      0xa6a6a6, 0x7e8da6, 0x8787ae, 0x9282ad, 0x9a7fa3, 0x9f8093, 0x9e8480, 0x988a71,
        0x8f9068, 0x859569, 0x7c9773, 0x789784, 0x789396, 0x707070, 0x000000, 0x000000 } };

// Points to the current palette as determined by the color tint bits
static uint32_t const*pal_to_rgb;

// Nametable memory of variable size, initialized when loading the ROM
uint8_t              *ciram;

static uint8_t        palettes[0x20];
static uint8_t        oam     [0x100];
static uint8_t        sec_oam [0x20];

// VRAM address and scroll regs
// Possible optimization: Make some of these a natural size for the
// implementation architecture
static uint16_t       t, v;
static uint8_t        fine_x;
// v is not immediately updated from t on the second write to $2006. This
// variable implements the delay.
static unsigned       pending_v_update;

static unsigned       v_inc;           // $2000:2
static uint16_t       sprite_pat_addr; // $2000:3
static uint16_t       bg_pat_addr;     // $2000:4
static Sprite_size    sprite_size;     // $2000:5
static bool           nmi_on_vblank;   // $2000:7

static uint8_t        grayscale_color_mask; // $2001:0 - 0x30 if grayscale mode enabled, otherwise 0x3F
static bool           show_bg_left_8;       // $2001:1
static bool           show_sprites_left_8;  // $2001:2
static bool           show_bg;              // $2001:3
static bool           show_sprites;         // $2001:4
static uint8_t        tint_bits;            // $2001:7-5

// Optimization - always equals show_bg || show_sprites
bool                  rendering_enabled;
// Optimizations - if bg/sprites are disabled, a value is set that causes
// comparisons to always fail. If the leftmost 8 pixels are clipped, the
// comparison will fail for those pixels. Otherwise, the comparison will never
// fail.
static unsigned       bg_clip_comp;
static unsigned       sprite_clip_comp;

static bool           sprite_overflow; // $2002:5
static bool           sprite_zero_hit; // $2002:6
static bool           in_vblank;       // $2002:7

static uint8_t        oam_addr;      // $2003
// Pointer into the secondary OAM, 5 bits wide
//  - Updated during sprite evaluation and loading
//  - Cleared at dots 64.5, 256.5 and 340.5, if rendering
static unsigned       sec_oam_addr;
static uint8_t        oam_data;      // $2004 (seen when reading from $2004)

// Sprite evaluation state

// Goes high for three ticks when an in-range sprite is found during sprite
// evaluation
static unsigned       copy_sprite_signal;
static bool           oam_addr_overflow, sec_oam_addr_overflow;
static bool           overflow_detection;

// PPUSCROLL/PPUADDR write flip-flop. First write when false, second write when
// true.
static bool           write_flip_flop;

// $2007 read buffer
static uint8_t        ppu_data_reg;

static bool           odd_frame;

// Used as a general-purpose timestamp throughout the emulator. Good for
// 109 000 years.
uint64_t              ppu_cycle;

// Internal PPU counters and registers

unsigned              dot, scanline;

static uint8_t        nt_byte, at_byte;
static uint8_t        bg_byte_l, bg_byte_h;
static uint16_t       bg_shift_l, bg_shift_h;
static unsigned       at_shift_l, at_shift_h;
static unsigned       at_latch_l, at_latch_h;

static uint8_t        sprite_attribs[8];
static uint8_t        sprite_positions[8];
static uint8_t        sprite_pat_l[8];
static uint8_t        sprite_pat_h[8];

static bool           s0_on_next_scanline;
static bool           s0_on_cur_scanline;

// Temporary storage (also exists in PPU) for data during sprite loading
static uint8_t        sprite_y, sprite_index;
static bool           sprite_in_range;

// Writes to certain registers are suppressed during the initial frame:
// http://wiki.nesdev.com/w/index.php/PPU_power_up_state
//
// Emulating this makes NY2011 and possibly other demos hang. They probably
// don't run on the real thing either.
static bool           initial_frame;

// VRAM address currently being output (MMC3 looks at this)
unsigned              ppu_addr_bus;

// Open bus for reads from PPU $2000-$2007 (tested by ppu_open_bus.nes)

static uint8_t        ppu_open_bus;
static uint64_t       ppu_bit_7_to_6_write_cycle, ppu_bit_5_write_cycle, ppu_bit_4_to_0_write_cycle;

// PPU open bus values fade after about 600 ms
unsigned const        open_bus_decay_cycles = 0.6*ntsc_ppu_clock_rate;

static void open_bus_refreshed() {
    ppu_bit_7_to_6_write_cycle = ppu_bit_5_write_cycle = ppu_bit_4_to_0_write_cycle = ppu_cycle;
}

static void open_bus_bits_7_to_5_refreshed() {
    ppu_bit_7_to_6_write_cycle = ppu_bit_5_write_cycle = ppu_cycle;
}

static void open_bus_bits_5_to_0_refreshed() {
    ppu_bit_5_write_cycle = ppu_bit_4_to_0_write_cycle = ppu_cycle;
}

static uint8_t get_open_bus_bits_7_to_6() {
    return (ppu_cycle - ppu_bit_7_to_6_write_cycle > open_bus_decay_cycles) ?
      0 : ppu_open_bus & 0xC0;
}

static uint8_t get_open_bus_bits_4_to_0() {
    return (ppu_cycle - ppu_bit_4_to_0_write_cycle > open_bus_decay_cycles) ?
      0 : ppu_open_bus & 0x1F;
}

static uint8_t get_all_open_bus_bits() {
    return
      get_open_bus_bits_7_to_6()                                                              |
      ((ppu_cycle - ppu_bit_5_write_cycle > open_bus_decay_cycles) ? 0 : ppu_open_bus & 0x20) |
      get_open_bus_bits_4_to_0();
}


static uint8_t &chr_ref(unsigned chr_addr) {
    return chr_pages[(chr_addr >> 10) & 7][chr_addr & 0x03FF];
}

// Nametable reading and writing
// Could factor out the address determination, but keep it straightforward and
// fast.

static uint8_t read_nt(uint16_t addr) {
    switch (mirroring) {
    case VERTICAL:        return ciram[addr & 0x07FF];                            break;
    case HORIZONTAL:      return ciram[((addr >> 1) & 0x0400) | (addr & 0x03FF)]; break;
    case ONE_SCREEN_LOW:  return ciram[addr & 0x03FF];                            break;
    case ONE_SCREEN_HIGH: return ciram[0x0400 | (addr & 0x03FF)];                 break;
    case FOUR_SCREEN:     return ciram[addr & 0x0FFF];                            break;
    case SPECIAL:         return mapper_read_nt(addr);                            break;
    default: UNREACHABLE
    }
}

static void write_nt(uint16_t addr, uint8_t value) {
    switch (mirroring) {
    case VERTICAL:        ciram[addr & 0x07FF]                            = value; break;
    case HORIZONTAL:      ciram[((addr >> 1) & 0x0400) | (addr & 0x03FF)] = value; break;
    case ONE_SCREEN_LOW:  ciram[addr & 0x03FF]                            = value; break;
    case ONE_SCREEN_HIGH: ciram[0x0400 | (addr & 0x03FF)]                 = value; break;
    case FOUR_SCREEN:     ciram[addr & 0x0FFF]                            = value; break;
    case SPECIAL:         mapper_write_nt(addr, value);                            break;
    default: UNREACHABLE
    }
}

static void bump_horiz() {
    // Coarse x equal to 31?
    // Another variant is 'if (!(~v & 0x001F))'.
    if (!((v + 1) & 0x1F))
        // Set coarse x to 0 and switch horizontal nametable. The bit twiddling
        // to clear the lower five bits relies on them being 1.
        v ^= 0x041F;
    else ++v;
}

static void bump_vert() {
    // Fine y != 7?
    if (~v & 0x7000) {
        // Bump fine y
        v += 0x1000;
        return;
    }

    unsigned coarse_y = (v >> 5) & 0x1F;
    switch (coarse_y) {
    case 29:
        // Switch vertical nametable
        v ^= 0x0800;
        // Fall-through
    case 31:
        coarse_y = 0;
        break;

    default:
        ++coarse_y;
    }

    // Clear fine y and put coarse y back into v
    v = (v & (~0x7000 & ~0x03E0)) | (coarse_y << 5);
}

static void copy_horiz() {
    // v: ... .H.. ...E DCBA = t: ... .H.. ...E DCBA
    v = (v & ~0x041F) | (t & 0x041F);
}

static void copy_vert() {
    // v: IHG F.ED CBA. .... = t: IHG F.ED CBA. ....
    v = (v & ~0x7BE0) | (t & 0x7BE0);
}

static void do_bg_fetches() {
    switch ((dot - 1) % 8) {
    case 0:
        ppu_addr_bus = 0x2000 | (v & 0x0FFF);
        break;

    case 1:
        nt_byte = read_nt(ppu_addr_bus);
        break;

    case 2:
        //    yyy NNAB CDEG HIJK
        // =>  10 NN11 11AB CGHI
        // 1162 is the Visual 2C02 signal that sets up this address
        ppu_addr_bus = 0x23C0 | (v & 0x0C00) | ((v >> 4) & 0x38) | ((v >> 2) & 7);
        break;

    case 3:
        at_byte = read_nt(ppu_addr_bus);
        break;

    case 4:
        assert(v <= 0x7FFF);
        ppu_addr_bus = bg_pat_addr + 16*nt_byte + (v >> 12);
        break;

    case 5:
        bg_byte_l = chr_ref(ppu_addr_bus);
        break;

    case 6:
        assert(v <= 0x7FFF);
        ppu_addr_bus = bg_pat_addr + 16*nt_byte + (v >> 12) + 8;
        break;

    case 7:
        bg_byte_h = chr_ref(ppu_addr_bus);
        bump_horiz();
        break;
    }
}

// Performance hotspot
// Possible optimization: Set flag if any sprites on the line
static unsigned get_sprite_pixel(unsigned &spr_pal, bool &spr_behind_bg, bool &spr_is_s0) {
    unsigned const pixel = dot - 2;
    // Equivalent to 'if (!show_sprites || (!show_sprites_left_8 && pixel < 8))'
    if (pixel < sprite_clip_comp)
        return 0;

    for (unsigned i = 0; i < 8; ++i) {
        unsigned const offset = pixel - sprite_positions[i];
        if (offset < 8) { // offset >= 0 && offset < 8
            unsigned const pat_res = (NTH_BIT(sprite_pat_h[i], 7 - offset) << 1) |
                                      NTH_BIT(sprite_pat_l[i], 7 - offset);
            if (pat_res) {
                spr_pal       = sprite_attribs[i] & 3;
                spr_behind_bg = sprite_attribs[i] & 0x20;
                spr_is_s0     = s0_on_cur_scanline && (i == 0);
                return pat_res;
            }
        }
    }

    return 0;
}

// Performance hotspot
static void do_pixel_output_and_sprite_0() {
    unsigned const pixel = dot - 2;
    unsigned pal_index;

    if (!rendering_enabled)
        // If v points in the $3Fxx range while rendering is disabled, the
        // color from that palette index is displayed instead of the background
        // color
        pal_index = (~v & 0x3F00) ? 0 : v & 0x1F;
    else {
        unsigned spr_pal;
        bool spr_behind_bg, spr_is_s0;
        unsigned const spr_pat = get_sprite_pixel(spr_pal, spr_behind_bg, spr_is_s0);

        unsigned bg_pixel_pat;
        // Equivalent to 'if (!show_bg || (!show_bg_left_8 && pixel < 8))'
        if (pixel < bg_clip_comp)
            bg_pixel_pat = 0;
        else {
            bg_pixel_pat = (NTH_BIT(bg_shift_h, 15 - fine_x) << 1) |
                            NTH_BIT(bg_shift_l, 15 - fine_x);

            if (spr_pat && spr_is_s0 && bg_pixel_pat && pixel != 255)
                sprite_zero_hit = true;
        }

        if (spr_pat && !(spr_behind_bg && bg_pixel_pat))
            pal_index = 0x10 + (spr_pal << 2) + spr_pat;
        else {
            if (!bg_pixel_pat)
                pal_index = 0;
            else {
                unsigned const attr_bits = (NTH_BIT(at_shift_h, 7 - fine_x) << 1) |
                                            NTH_BIT(at_shift_l, 7 - fine_x);
                pal_index = (attr_bits << 2) | bg_pixel_pat;
            }
        }
    }

    put_pixel(pixel, scanline, pal_to_rgb[palettes[pal_index] & grayscale_color_mask]);
}

static void do_shifts_and_reloads() {
    assert(at_latch_l <= 1);
    assert(at_latch_h <= 1);

    bg_shift_l <<= 1;
    bg_shift_h <<= 1;
    at_shift_l = (at_shift_l << 1) | at_latch_l;
    at_shift_h = (at_shift_h << 1) | at_latch_h;

    if (dot % 8 == 1) {
        // Reload regs
        bg_shift_l = (bg_shift_l & 0xFF00) | bg_byte_l;
        bg_shift_h = (bg_shift_h & 0xFF00) | bg_byte_h;

        // v:
        //
        // 432 10 98765 43210
        // yyy NN YYYYY XXXXX
        // ||| || ||||| +++++-- coarse X scroll
        // ||| || +++++-------- coarse Y scroll
        // ||| ++-------------- nametable select
        // +++----------------- fine Y scroll
        //
        // v as bytes:
        // 432 1098 7654 3210
        // yyy NNYY YYYX XXXX
        //
        // http://wiki.nesdev.com/w/index.php/PPU_attribute_tables
        // ,---+---+---+---.
        // |   |   |   |   |
        // + D1-D0 + D3-D2 +
        // |   |   |   |   |
        // +---+---+---+---+
        // |   |   |   |   |
        // + D5-D4 + D7-D6 +
        // |   |   |   |   |
        // `---+---+---+---'

        // Equivalent to the following:
        // unsigned const coarse_x = v & 0x1F;
        // unsigned const coarse_y = (v >> 5) & 0x1F;
        // unsigned const at_bits = at_byte >> 2*((coarse_y & 0x02) | (((coarse_x - 1) & 0x02) >> 1));
        unsigned const at_bits = at_byte >> (((v >> 4) & 4) | ((v - 1) & 2));

        at_latch_l = at_bits & 1;
        at_latch_h = (at_bits >> 1) & 1;
    }
}

static void move_to_next_oam_byte() {
    oam_addr     = (oam_addr     + 1) & 0xFF;
    sec_oam_addr = (sec_oam_addr + 1) & 0x1F;

    if (oam_addr == 0)
        oam_addr_overflow = true;

    if (sec_oam_addr == 0) {
        sec_oam_addr_overflow = true;
        // If sec_oam_addr becomes zero, eight sprites have been found, and we
        // enter overflow glitch mode
        overflow_detection = true;
    }
}

static void do_sprite_evaluation() {
    if (dot == 65) {
        // TODO: Should these be cleared even if rendering is disabled?
        overflow_detection = oam_addr_overflow = sec_oam_addr_overflow = false;
        sec_oam_addr = 0;
    }

    if (dot & 1) {
        // On odd ticks, data is read from OAM
        oam_data = oam[oam_addr];
    }
    else {
        // We need the original value to implement sprite overflow checking. It
        // might get overwritten below.
        uint8_t const orig_oam_data = oam_data;

        // On even ticks, data is written into secondary OAM...
        if (!(oam_addr_overflow || sec_oam_addr_overflow))
            sec_oam[sec_oam_addr] = oam_data;
        else
            // ...unless we have OAM or secondary OAM overflow, in which case we
            // get a read from secondary OAM instead
            oam_data = sec_oam[sec_oam_addr];

        if (copy_sprite_signal > 0) {
            --copy_sprite_signal;
            move_to_next_oam_byte();
        }
        else {
            bool const in_range =
              (scanline - orig_oam_data) < (sprite_size == EIGHT_BY_EIGHT ? 8 : 16);
            // At dot 66 we're evaluating the first sprite. This is how the
            // hardware does it.
            if (dot == 66)
                s0_on_next_scanline = in_range;
            if (in_range && !(oam_addr_overflow || sec_oam_addr_overflow)) {
                copy_sprite_signal = 3;
                move_to_next_oam_byte();
            }
            else {
                if (!overflow_detection) {
                    // Clear low bits, bump high (HW does this, even though the low
                    // clearing wouldn't usually be noticeable)
                    oam_addr = (oam_addr + 4) & 0xFC;
                    if (oam_addr == 0)
                        oam_addr_overflow = true;
                }
                else {
                    if (in_range && !oam_addr_overflow) {
                        sprite_overflow = true;
                        overflow_detection = false;
                    }
                    else {
                        // Glitchy oam_addr increment after exactly eight
                        // sprites have been found:
                        // http://wiki.nesdev.com/w/index.php/PPU_sprite_evaluation
                        oam_addr = ((oam_addr + 4) & 0xFC) | ((oam_addr + 1) & 3);
                        if ((oam_addr & 0xFC) == 0)
                            oam_addr_overflow = true;
                    }
                }
            }
        }
    }
}

// Returns 'true' if the sprite is in range
static bool calculate_sprite_tile_address(uint8_t y, uint8_t index, uint8_t attrib, bool is_high) {
    // Internal sprite address calculation in the PPU (ab = VRAM address bus):
    //
    //   ab12  : low bit of sprite index if using 8x16 sprites, otherwise
    //           sprite_pat_addr ($2000:3)
    //
    //   ab11-5: Bits 7-1 of sprite index
    //
    //   ab4   : Bit 3 of scanline - y after possible y-flip if 8x16,
    //           otherwise low bit of sprite index
    //
    //   ab3   : 0 if fetching the low tile, otherwise 1. Determined by
    //           horizontal position in the hardware.
    //
    //   ab2-0 : Bits 2-0 of scanline - y, possibly y-flipped
    unsigned const diff        = scanline - y;
    unsigned const diff_y_flip = (attrib & 0x80) ? ~diff : diff;

    if (sprite_size == EIGHT_BY_EIGHT) {
        ppu_addr_bus = sprite_pat_addr + 16*index + 8*is_high + (diff_y_flip & 7);
        // Equivalent to diff >= 0 && diff < 8 due to unsigned arithmetic
        return diff < 8;
    }
    else { // EIGHT_BY_SIXTEEN
        ppu_addr_bus = 0x1000*(index & 1) + 16*(index & 0xFE) + ((diff_y_flip & 8) << 1)
                                          + 8*is_high + (diff_y_flip & 7);
        return diff < 16;
    }
}

static void do_sprite_loading() {
    // This is position-based in the hardware as well
    #define SPRITE_N ((dot - 257)/8)

    // Sprite 0 flag timing:
    //  - s0_on_next_scanline is initialized at dot = 66.5-67 (during sprite evaluation for sprite 0)
    //  - It is copied over to s0_on_cur_scanline at dot 257.5-258, 258.5-259, ..., 319.5-320
    s0_on_cur_scanline = s0_on_next_scanline;

    if (dot == 257)
        sec_oam_addr = 0;

    switch ((dot - 1) % 8) {
    case 0:
        // TODO: How does the sprite_y/index loading work in detail?

        // Dummy NT fetch. Not 100% confirmed, but probably uses the same
        // address as the BG fetching logic at this location.
        ppu_addr_bus = 0x2000 | (v & 0x0FFF);

        sprite_y = sec_oam[sec_oam_addr];
        sec_oam_addr = (sec_oam_addr + 1) & 0x1F;
        break;

    case 1:
        sprite_index = sec_oam[sec_oam_addr];
        sec_oam_addr = (sec_oam_addr + 1) & 0x1F;
        break;

    case 2:
        // The "dummy AT fetch" during sprite loading is an NT fetch too
        ppu_addr_bus = 0x2000 | (v & 0x0FFF);

        sprite_attribs[SPRITE_N] = sec_oam[sec_oam_addr];
        sec_oam_addr = (sec_oam_addr + 1) & 0x1F;
        break;

    case 3:
        sprite_positions[SPRITE_N] = sec_oam[sec_oam_addr];
        sec_oam_addr = (sec_oam_addr + 1) & 0x1F;
        break;

    case 4:
        sprite_in_range = calculate_sprite_tile_address(sprite_y, sprite_index, sprite_attribs[SPRITE_N], false);
        break;

    case 5:
        {
        unsigned const sprite_n = SPRITE_N;
        sprite_pat_l[sprite_n] = sprite_in_range ? chr_ref(ppu_addr_bus) : 0;
        // Horizontal flipping
        if (sprite_attribs[sprite_n] & 0x40)
            sprite_pat_l[sprite_n] = rev_byte(sprite_pat_l[sprite_n]);
        break;
        }

    case 6:
        sprite_in_range = calculate_sprite_tile_address(sprite_y, sprite_index, sprite_attribs[SPRITE_N], true);
        break;

    case 7:
        {
        unsigned const sprite_n = SPRITE_N;
        sprite_pat_h[sprite_n] = sprite_in_range ? chr_ref(ppu_addr_bus) : 0;
        // Horizontal flipping
        if (sprite_attribs[sprite_n] & 0x40)
            sprite_pat_h[sprite_n] = rev_byte(sprite_pat_h[sprite_n]);

        break;
        }

    default: UNREACHABLE
    }

    #undef SPRITE_N
}

// Performance hotspot
static void do_prerender_and_visible_lines_ops() {
    // We get a short dummy bg-related fetch here. Probably not worth
    // emulating the exact address.
    // TODO: This breaks mmc3_test_2 - look into it more
    //if (dot == 0) ppu_addr_bus = bg_pat_addr;

    if ((dot >= 2 && dot <= 257) || (dot >= 322 && dot <= 337))
        do_shifts_and_reloads();

    switch (dot) {
    case 1 ... 256: case 321 ... 336:
        // Possible optimization: Could be merged to save double decoding of dot
        do_bg_fetches();
        if (dot == 256)
            bump_vert();
        break;

    case 257 ... 320:
        // Possible optimization: Could be merged to save double decoding of dot
        do_sprite_loading();
        oam_addr = 0;
        if (dot == 257)
            copy_horiz();
        break;

    case 337: case 339:
        // Dummy NT fetches
        ppu_addr_bus = 0x2000 | (v & 0xFFF);
        break;

    case 341:
        sec_oam_addr = 0;
        break;
    }
}

// Performance hotspot
void tick_ppu() {
    // Move to next tick - doing this first mirrors how Visual 2C02 views it
    if (++dot == 341) {
        dot = 0;
        ++scanline;
        // Possible optimization: set an enum indicating the scanline range
        // here and use below (SCANLINE_0_TO_239, SCANLINE_241, etc.)
        switch (scanline) {
        case 240:
            pending_event = pending_frame_completion = true;
            break;

        case 262:
            scanline = 0;
            if (rendering_enabled && odd_frame) ++dot;
            odd_frame = !odd_frame;
        }
    }

    if (pending_v_update > 0 && --pending_v_update == 0)
        v = t;

    ++ppu_cycle;

    switch (scanline) {
    case 0 ... 239:
        if (dot >= 2 && dot <= 257)
            do_pixel_output_and_sprite_0();

        if (rendering_enabled) {
            do_prerender_and_visible_lines_ops();

            // Secondary OAM clear
            //
            // Timing:
            // Dots 1-64:
            // Odd cycles: Read $FF, increment sec_oam_addr
            // Even cycles: Write
            // Lasts for the entire cycle
            if (dot >= 1 && dot <= 64) {
                if (dot & 1)
                    oam_data = 0xFF;
                else {
                    sec_oam[sec_oam_addr] = oam_data;
                    // Is this visible in any way?
                    sec_oam_addr = (sec_oam_addr + 1) & 0x1F;
                }
            }

            if (dot >= 65 && dot <= 256)
                do_sprite_evaluation();
        }

        break;

    case 241:
        if (dot == 1) {
            in_vblank = true;
            nmi_asserted = nmi_on_vblank;
        }
        break;

    case 261:
        // This might be one tick off due to the possibility of reading the
        // flags really shortly after they are cleared in the preferred
        // alignment
        if (dot == 1)
            sprite_overflow = sprite_zero_hit = initial_frame = false;
        // TODO: Explain why the timing works out like this (and is it cycle-perfect?)
        if (dot == 2)
            in_vblank = false;

        if (rendering_enabled) {
            // This is where s0_on_next_scanline is initialized on the
            // prerender line the hardware. There's an "in visible frame"
            // condition on the value the flag is initialized to - hence it
            // always becomes false.
            if (dot == 66)
                s0_on_next_scanline = false;

            do_prerender_and_visible_lines_ops();

            if (dot >= 280 && dot <= 304)
                copy_vert();
        }

        break;
    }

    // TODO: ppu_addr_bus could be updated as needed when v changes
    if ((scanline >= 240 && scanline <= 260) || !rendering_enabled)
        ppu_addr_bus = v & 0x3FFF;

    ppu_tick_callback();
}

static void do_2007_post_access_bump() {
    if (rendering_enabled && (scanline < 240 || scanline > 260)) {
        // Accessing $2007 during rendering performs this glitch. Used by Young
        // Indiana Jones Chronicles to shake the screen.
        bump_horiz();
        bump_vert();
    }
    // The incrementation operation can touch the high bit even though it's not
    // used for addressing (it's the high bit of fine y)
    else v = (v + v_inc) & 0x7FFF;
}

uint8_t read_ppu() {
    LOG_PPU_MEM("Read from PPU $%04X ", v);

    // Use ppu_open_bus to hold the result, updating it in the process

    switch (v & 0x3FFF) {
    case 0x0000 ... 0x1FFF:
        LOG_PPU_MEM("(Pattern tables, $0000-$1FFF)\n");
        ppu_open_bus = ppu_data_reg;
        open_bus_refreshed();
        ppu_data_reg = chr_ref(v);
        break;

    case 0x2000 ... 0x3EFF:
        LOG_PPU_MEM("(Nametables, $2000-$3EFF)");
        ppu_open_bus = ppu_data_reg;
        open_bus_refreshed();
        ppu_data_reg = read_nt(v);
        break;

    case 0x3F00 ... 0x3FFF:
        LOG_PPU_MEM("(Palettes, $3F00-$3FFF)\n");
        ppu_open_bus = get_open_bus_bits_7_to_6() |
          (palettes[v & 0x1F] & grayscale_color_mask);
        open_bus_bits_5_to_0_refreshed();
        ppu_data_reg = read_nt(v);
        break;

    // GCC doesn't seem to infer this
    default: UNREACHABLE
    }

    if (initial_frame) {
        ppu_data_reg = 0;
        printf("Warning: Reading PPUDATA during initial frame, at (%u,%u)\n", scanline, dot);
    }

    do_2007_post_access_bump();
    return ppu_open_bus;
}

void write_ppu(uint8_t value) {
    LOG_PPU_MEM("Write $%02X to PPU $%04X ", value, v);

    switch (v & 0x3FFF) {

    case 0x0000 ... 0x1FFF:
        LOG_PPU_MEM("(Pattern tables, $0000-$1FFF)\n");
        if (uses_chr_ram) chr_ref(v) = value;
        break;

    case 0x2000 ... 0x3EFF:
        {
        LOG_PPU_MEM("(Nametables, $2000-$3EFF)\n");
        write_nt(v, value);
        break;
        }

    case 0x3F00 ... 0x3FFF:
        {
        LOG_PPU_MEM("(Palettes, $3F00-$3F1F)\n");

        // As the palette is read much more often than it is written, we
        // simulate mirroring by actually writing the mirrored values. For
        // unmirrored cell, the mirror map points back to the cell itself.

        static uint8_t const palette_write_mirror[0x20] =
          { 0x10, 0x01, 0x02, 0x03, 0x14, 0x05, 0x06, 0x07,
            0x18, 0x09, 0x0A, 0x0B, 0x1C, 0x0D, 0x0E, 0x0F,
            0x00, 0x11, 0x12, 0x13, 0x04, 0x15, 0x16, 0x17,
            0x08, 0x19, 0x1A, 0x1B, 0x0C, 0x1D, 0x1E, 0x1F };

        palettes[palette_write_mirror[v & 0x1F]] = palettes[v & 0x001F] = value & 0x3F;
        break;
        }

    // GCC doesn't seem to infer this
    default: UNREACHABLE
    }

    do_2007_post_access_bump();
}

uint8_t read_ppu_reg(unsigned n) {
    switch (n) {

    case 0:
        LOG_PPU_REG("Read PPUCTRL ($2000)\n");
        return get_all_open_bus_bits();

    case 1:
        LOG_PPU_REG("Read PPUMASK ($2001)\n");
        return get_all_open_bus_bits();

    case 2:
        {
        LOG_PPU_REG("Read PPUSTATUS ($2002)\n");
        if (scanline == 241) {
            // Quirkiness related to reading $2002 around the point where the
            // VBlank flag is set. TODO: Explain why these values are correct.
            switch (dot) {
            case 1:
                in_vblank    = false;
                nmi_asserted = false;
                break;

            case 2: case 3:
                nmi_asserted = false;
                break;
            }
        }
        write_flip_flop = false;
        ppu_open_bus = (in_vblank << 7) | (sprite_zero_hit << 6) | (sprite_overflow << 5) |
                       get_open_bus_bits_4_to_0();
        open_bus_bits_7_to_5_refreshed();
        in_vblank = false;
        return ppu_open_bus;
        }

    case 3:
        LOG_PPU_REG("Read OAMADDR ($2003)\n");
        return get_all_open_bus_bits();

    case 4:
        LOG_PPU_REG("Read OAMDATA ($2004)\n");
        // Micro machines reads this during rendering
        if (rendering_enabled && (scanline < 240 || scanline > 260)) {
            // TODO: Make this work automagically through proper emulation of
            // the interval after the sprite fetches
            if (dot >= 323)
                return sec_oam[0];
            return oam_data;
        }
        open_bus_refreshed();
        return ppu_open_bus = ((oam_addr & 3) == 2) ? (oam[oam_addr] & 0xE3) : oam[oam_addr];

    case 5:
        LOG_PPU_REG("Read PPUSCROLL ($2005)\n");
        return get_all_open_bus_bits();

    case 6:
        LOG_PPU_REG("Read PPUADDR ($2006)\n");
        return get_all_open_bus_bits();

    case 7:
        LOG_PPU_REG("Read PPUDATA ($2007)\n");
        return read_ppu();

    default: UNREACHABLE
    }
}

// $2004
void write_oam_data_reg(uint8_t value) {
    // OAM updates are inhibited during rendering. $2004 writes during
    // rendering do perform a glitchy oam_addr increment however, but that
    // might be hard to pin down (could depend on current sprite evaluation
    // status for example) and not worth emulating.
    if (rendering_enabled && (scanline < 240 || scanline > 260))
        return;
    oam[oam_addr++] = value;
}

void write_ppu_reg(uint8_t value, unsigned n) {
    ppu_open_bus = value;
    open_bus_refreshed();

    switch (n) {

    case 0: // PPUCTRL
        LOG_PPU_REG("Write $%02X to PPUCTRL ($2000)\n", value);

        {
        if (initial_frame) {
            printf("Warning: Writing PPUCTRL during initial frame, at (%u,%u)\n", scanline, dot);
            return;
        }

        // t: ... AB.. .... .... = value: .... ..AB
        t               = (t & 0x73FF) | ((value & 0x03) << 10);
        v_inc           = (value & 0x04) ? 32 : 1;
        sprite_pat_addr = (value & 0x08) << 9; // value & 0x08 ? 0x1000 : 0x0000
        bg_pat_addr     = (value & 0x10) << 8; // value & 0x10 ? 0x1000 : 0x0000
        sprite_size     = value & 0x20 ? EIGHT_BY_SIXTEEN : EIGHT_BY_EIGHT;

        bool const new_nmi_on_vblank = value & 0x80;
        if (new_nmi_on_vblank) {
            // An unset-to-set transition in nmi_on_vblank while in_vblank is
            // set causes another NMI to be generated, since the NMI line
            // equals nmi_on_vblank AND in_vblank (though it's active low
            // instead): http://wiki.nesdev.com/w/index.php/NMI
            if (!nmi_on_vblank && in_vblank)
                nmi_asserted = true;
        }
        else
            // This ensures that no NMI is generated if NMIs are disabled right
            // around where the vblank flag is set. We might get a short NMI
            // pulse in that case, but it won't be seen.
            nmi_asserted = false;

        nmi_on_vblank = new_nmi_on_vblank;

        break;
        }

    case 1: // PPUMASK
        LOG_PPU_REG("Write $%02X to PPUMASK ($2001)\n", value);

        if (initial_frame) {
            printf("Warning: Writing PPUMASK during initial frame, at (%u,%u)\n", scanline, dot);
            return;
        }

        grayscale_color_mask = value & 0x01 ? 0x30 : 0x3F;
        show_bg_left_8       = value & 0x02;
        show_sprites_left_8  = value & 0x04;
        show_bg              = value & 0x08;
        show_sprites         = value & 0x10;
        tint_bits            = (value >> 5) & 7;

        // The status of the tint bits determines the current palette
        pal_to_rgb = nes_to_rgb[tint_bits];

        // Optimizations
        rendering_enabled = show_bg || show_sprites;
        bg_clip_comp      = !show_bg      ? 256 : show_bg_left_8      ? 0 : 8;
        sprite_clip_comp  = !show_sprites ? 256 : show_sprites_left_8 ? 0 : 8;

        break;

    case 2: // PPUSTATUS
        LOG_PPU_REG("Write $%02X to PPUSTATUS ($2002)\n", value);
        // Read-only
        break;

    case 3: // OAMADDR
        LOG_PPU_REG("Write $%02X to OAMADDR ($2003)\n", value);
        oam_addr = value;
        break;

    case 4: // OAMDATA
        LOG_PPU_REG("Write $%02X to OAMDATA ($2004)\n", value);
        write_oam_data_reg(value);
        break;

    case 5: // PPUSCROLL
        LOG_PPU_REG("Write $%02X to PPUSCROLL ($2005), at (%u,%u)\n", value, scanline, dot);

        if (initial_frame) {
            printf("Warning: Writing PPUSCROLL during initial frame, at (%u,%u)\n", scanline, dot);
            return;
        }

        if (!write_flip_flop) {
            // First write
            // fine_x = value: .... .ABC
            // t: ... .... ...D EFGH = value: DEFG H...
            fine_x = value & 7;
            t = (t & 0x7FE0) | ((value & 0xF8) >> 3);
        }
        else
            // Second write
            // t: ABC ..DE FGH. .... = value: DEFG HABC
            t = (t & 0x0C1F) | ((value & 0xF8) << 2) | ((value & 7) << 12);

        write_flip_flop = !write_flip_flop;
        break;

    case 6: // PPUADDR
        LOG_PPU_REG("Write $%02X to PPUADDR ($2006), at (%u,%u)\n", value, scanline, dot);

        if (initial_frame) {
            printf("Warning: Writing PPUADDR during initial frame, at (%u,%u)\n", scanline, dot);
            return;
        }

        if (!write_flip_flop)
            // First write
            // t: 0AB CDEF .... .... = value: ..AB CDEF
            // Clearing of high bit confirmed in Visual 2C02
            t = (t & 0x00FF) | ((value & 0x3F) << 8);
        else {
            // Second write
            // t: ... .... ABCD EFGH = value: ABCD EFGH
            t = (t & 0x7F00) | value;
            // There is a delay of ~3 ticks before t is copied to v
            pending_v_update = 3;
        }

        write_flip_flop = !write_flip_flop;
        break;

    case 7: // PPUDATA
        LOG_PPU_REG("Write $%02X to PPUDATA ($2007), at (%u,%u)\n", value, scanline, dot);
        write_ppu(value);
        break;

    default: UNREACHABLE
    }
}

// Helpers for setting the cold boot state and resetting
// TODO: Make set_ppu_cold_boot_state() use reset() for things reset by the
// reset signal

static void clear_2000() {
    // $2000
    v_inc           = 1;
    sprite_pat_addr = bg_pat_addr = 0x0000;
    sprite_size     = EIGHT_BY_EIGHT;
    nmi_on_vblank   = false;
}

static void clear_2001() {
    grayscale_color_mask = 0x3F; // Grayscale off
    show_bg_left_8       = show_sprites_left_8 = false;
    show_bg              = show_sprites        = false;
    tint_bits            = 0;
    pal_to_rgb           = nes_to_rgb[tint_bits];
    rendering_enabled    = false;
    bg_clip_comp         = sprite_clip_comp = 256;
}

void set_ppu_cold_boot_state() {
    // CIRAM is cleared when loading the ROM, at the same time that we
    // determine the CIRAM size

    // This makes the uninitialized background color the "NES gray" seen on
    // startup in many games, so it's probably correct(ish)
    init_array(palettes, (uint8_t)0);
    // Make all sprites out-of-range by default to prevent temporary glitching.
    // On the real thing the values might be indeterminate.
    init_array(oam    , (uint8_t)0xFF);
    init_array(sec_oam, (uint8_t)0xFF);

    // Loopy regs
    fine_x = t = v = 0;

    clear_2000();
    clear_2001();

    // $2002
    sprite_overflow = sprite_zero_hit = in_vblank = false;

    // OAM regs
    oam_addr = sec_oam_addr = oam_data = 0;

    // Sprite evaluation state

    copy_sprite_signal = 0;
    oam_addr_overflow  = sec_oam_addr_overflow = false;
    overflow_detection = false;

    // Misc. regs and helpers
    write_flip_flop     = false;
    ppu_data_reg        = 0;
    pending_v_update    = 0;     // No pending v update
    odd_frame           = false; // Initial frame is even
    initial_frame       = starts_on_initial_frame;
    s0_on_next_scanline = s0_on_cur_scanline = false;
    ppu_addr_bus        = 0;
    dot                 = scanline = ppu_cycle = 0;

    // Open bus

    ppu_open_bus = 0;
    ppu_bit_7_to_6_write_cycle = ppu_bit_5_write_cycle = ppu_bit_4_to_0_write_cycle = 0;

    // Render pipeline buffers and shift registers and sprite output units

    nt_byte    = at_byte    = 0;
    bg_byte_l  = bg_byte_h  = 0;
    bg_shift_l = bg_shift_h = 0;
    at_shift_l = at_shift_h = 0;
    at_latch_l = at_latch_h = 0;

    sprite_y = sprite_index = 0;
    sprite_in_range = false;

    init_array(sprite_attribs  , (uint8_t)0);
    init_array(sprite_positions, (uint8_t)0);
    init_array(sprite_pat_l    , (uint8_t)0);
    init_array(sprite_pat_h    , (uint8_t)0);
}

void reset_ppu() {
    // Loopy regs
    fine_x = t = 0;

    clear_2000();
    clear_2001();

    // 2002 is probably unchanged since the reset signal isn't tied to any of
    // the flip-flops

    write_flip_flop = false;
    dot = scanline = 0;
    odd_frame = false;

    sprite_y = sprite_index = 0;
    sprite_in_range = false;
}

// State transfers

template<bool calculating_size, bool is_save>
void transfer_ppu_state(uint8_t *&buf) {
    #define T(x) transfer<calculating_size, is_save>(x, buf);
    #define T_MEM(x, len) transfer_mem<calculating_size, is_save>(x, len, buf);

    if (uses_chr_ram) T_MEM(chr_base, 0x2000);
    T_MEM(ciram, mirroring == FOUR_SCREEN ? 0x1000 : 0x800);
    T(palettes)
    T(oam) T(sec_oam)
    T(t) T(v) T(fine_x)
    T(pending_v_update)

    T(v_inc)
    T(sprite_pat_addr)
    T(bg_pat_addr)
    T(sprite_size)
    T(nmi_on_vblank)

    T(grayscale_color_mask)
    T(show_bg_left_8)
    T(show_sprites_left_8)
    T(show_bg)
    T(show_sprites)
    T(tint_bits)
    if (!is_save) {
        rendering_enabled = show_bg || show_sprites;
        // TODO: Factor out helper to avoid having this in two locations
        bg_clip_comp      = !show_bg      ? 256 : show_bg_left_8      ? 0 : 8;
        sprite_clip_comp  = !show_sprites ? 256 : show_sprites_left_8 ? 0 : 8;
        pal_to_rgb = nes_to_rgb[tint_bits];
    }

    T(sprite_overflow) T(sprite_zero_hit) T(in_vblank)

    T(oam_addr) T(sec_oam_addr) T(oam_data)

    T(copy_sprite_signal)
    T(oam_addr_overflow) T(sec_oam_addr_overflow)
    T(overflow_detection)

    T(write_flip_flop)
    T(ppu_data_reg)
    T(odd_frame)
    T(ppu_cycle)

    T(dot) T(scanline)

    T(nt_byte) T(at_byte)
    T(bg_byte_l) T(bg_byte_h)
    T(bg_shift_l) T(bg_shift_h)
    T(at_shift_l) T(at_shift_h)
    T(at_latch_l) T(at_latch_h)

    T(sprite_attribs)
    T(sprite_positions)
    T(sprite_pat_l)
    T(sprite_pat_h)

    T(s0_on_next_scanline)
    T(s0_on_cur_scanline)

    T(sprite_y) T(sprite_index)
    T(sprite_in_range)

    T(initial_frame)

    T(ppu_addr_bus)

    T(ppu_open_bus)
    T(ppu_bit_7_to_6_write_cycle) T(ppu_bit_5_write_cycle) T(ppu_bit_4_to_0_write_cycle)

    #undef T
    #undef T_MEM
}

// Explicit instantiations

// Calculating state size
template void transfer_ppu_state<true, false>(uint8_t*&);
// Saving state to buffer
template void transfer_ppu_state<false, true>(uint8_t*&);
// Loading state from buffer
template void transfer_ppu_state<false, false>(uint8_t*&);
