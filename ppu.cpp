// PPU (graphics processor) emulation. Follows the model in
// http://wiki.nesdev.com/w/images/d/d1/Ntsc_timing.png.
//
// Relevant pages:
//   http://wiki.nesdev.com/w/index.php/PPU_registers
//   http://wiki.nesdev.com/w/index.php/PPU_rendering
//   http://wiki.nesdev.com/w/index.php/The_skinny_on_NES_scrolling

#include "common.h"

#include "cpu.h"
#include "ppu.h"
#include "mapper.h"
#include "rom.h"
#include "sdl_backend.h"
#include "timing.h"

#include "palette.inc"

// Points to the current palette as determined by the color tint bits
static uint32_t const*pal_to_rgb;

// Set true at the end of the visible part of the frame to trigger end-of-frame
// operations at the next instruction boundary. This simplifies state transfers
// as the current location within the CPU emulation loop is part of the state.
bool                  pending_frame_completion;

// If true, treat the emulated code as the first code that runs (i.e., not the
// situation on PowerPak), which means writes to certain registers will be
// inhibited during the initial frame. This breaks some demos.
bool const            starts_on_initial_frame = false;

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

static uint8_t        oam_addr;        // $2003
// Pointer into the secondary OAM, 5 bits wide
//  - Updated during sprite evaluation and loading
//  - Cleared at dots 64.5, 256.5 and 340.5, if rendering
static unsigned       sec_oam_addr;
static uint8_t        oam_data;        // $2004 (seen when reading from $2004)

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
    case SPECIAL:         mapper_write_nt(value, addr);                            break;
    default: UNREACHABLE
    }
}

// Bumps the horizontal bits in v every eight pixels during rendering
static void bump_horiz() {
    // Coarse x equal to 31?
    if ((v & 0x1F) == 0x1F)
        // Set coarse x to 0 and switch horizontal nametable. The bit twiddling
        // to clear the lower five bits relies on them being 1.
        v ^= 0x041F;
    else ++v;
}

// Bumps the vertical bits in v at the end of each scanline during rendering
static void bump_vert() {
    // Fine y != 7?
    if (~v & 0x7000) {
        // Bump fine y
        v += 0x1000;
        return;
    }

    // Bump coarse y

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

// Restores the horizontal bits in v from t at the end of each scanline during
// rendering
static void copy_horiz() {
    // v: ... .H.. ...E DCBA = t: ... .H.. ...E DCBA
    v = (v & ~0x041F) | (t & 0x041F);
}

// Initializes the vertical bits in v from t on the pre-render line (line 261)
static void copy_vert() {
    // v: IHG F.ED CBA. .... = t: IHG F.ED CBA. ....
    v = (v & ~0x7BE0) | (t & 0x7BE0);
}

// Fetches nametable and tile bytes for the background
static void do_bg_fetches() {
    switch ((dot - 1) % 8) {

    // NT byte
    case 0: ppu_addr_bus = 0x2000 | (v & 0x0FFF); break;
    case 1: nt_byte = read_nt(ppu_addr_bus);      break;

    // AT byte
    case 2:
        //    yyy NNAB CDEG HIJK
        // =>  10 NN11 11AB CGHI
        // 1162 is the Visual 2C02 signal that sets up this address
        ppu_addr_bus = 0x23C0 | (v & 0x0C00) | ((v >> 4) & 0x38) | ((v >> 2) & 7);
        break;
    case 3:
        at_byte = read_nt(ppu_addr_bus);
        break;

    // Low BG tile byte
    case 4:
        assert(v <= 0x7FFF);
        ppu_addr_bus = bg_pat_addr + 16*nt_byte + (v >> 12);
        break;
    case 5:
        bg_byte_l = chr_ref(ppu_addr_bus);
        break;

    // High BG tile byte and horizontal bump
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

// Looks for an in-range sprite pixel at the current location
// Performance hotspot!
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

// Fetches pixels from the background and sprite shift registers and produces
// an output pixel according to the pixel values and background/sprite
// priority. Also handles sprite zero hit detection.
// Performance hotspot!
static void do_pixel_output_and_sprite_0() {
    unsigned const pixel = dot - 2;
    unsigned pal_index;

    if (!rendering_enabled)
        // If v points in the $3Fxx range while rendering is disabled, the
        // color from that palette index is displayed instead of the background
        // color
        pal_index = (~v & 0x3F00) ? 0 : v & 0x1F;
    else {
        unsigned       bg_pixel_pat;

        bool           spr_behind_bg, spr_is_s0;
        unsigned       spr_pal;
        unsigned const spr_pat = get_sprite_pixel(spr_pal, spr_behind_bg, spr_is_s0);

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

// Shifts the background shift registers, reloading the upper eight bits and
// the attribute bits every eight pixels
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

// Bumps the OAM and secondary OAM addresses, detecting overflow in either one
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

// Performs sprite evaluation for the next scanline, during dots 65-256. A
// linear search of the primary OAM is performed, and sprites found to be
// within range are copied into the secondary OAM.
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

// Initializes the sprite output units with the sprites that were copied into
// the secondary OAM during sprite evaluation
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

// Common operations for the visible lines (0-239) and the pre-render line (261)
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

// Runs the PPU for one dot
// Performance hotspot - ticks at ~5.4 MHz
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
            // The PPU address bus mirrors v outside of rendering
            ppu_addr_bus = v & 0x3FFF;
            break;

        case 262:
            scanline = 0;
            if (rendering_enabled && odd_frame) ++dot;
            odd_frame = !odd_frame;
        }
    }

    if (pending_v_update > 0 && --pending_v_update == 0) {
        v = t;
        if ((scanline >= 240 && scanline <= 260) || !rendering_enabled)
            // The PPU address bus mirrors v outside of rendering
            ppu_addr_bus = v & 0x3FFF;
    }

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
    else {
        v = (v + v_inc) & 0x7FFF;
        // The PPU address bus mirrors v outside of rendering
        ppu_addr_bus = v & 0x3FFF;
    }
}

static uint8_t read_vram() {
    // Use ppu_open_bus to hold the result, updating it in the process

    switch (v & 0x3FFF) {

    // Pattern tables
    case 0x0000 ... 0x1FFF:
        ppu_open_bus = ppu_data_reg;
        open_bus_refreshed();
        ppu_data_reg = chr_ref(v);
        break;

    // Nametables
    case 0x2000 ... 0x3EFF:
        ppu_open_bus = ppu_data_reg;
        open_bus_refreshed();
        ppu_data_reg = read_nt(v);
        break;

    // Palettes
    case 0x3F00 ... 0x3FFF:
        ppu_open_bus = get_open_bus_bits_7_to_6() |
          (palettes[v & 0x1F] & grayscale_color_mask);
        open_bus_bits_5_to_0_refreshed();

        // The data register is updated with the nametable byte that would
        // appear "underneath" the palette
        // (http://wiki.nesdev.com/w/index.php/PPU_memory_map)
        ppu_data_reg = read_nt(v);
        break;

    // GCC doesn't seem to infer this
    default: UNREACHABLE
    }

    if (initial_frame) {
        ppu_data_reg = 0;
        printf("Warning: Reading PPUDATA during initial frame, at (%u,%u)\n", scanline, dot);
    }

    return ppu_open_bus;
}

static void write_vram(uint8_t value) {
    switch (v & 0x3FFF) {

    // Pattern tables
    case 0x0000 ... 0x1FFF: if (uses_chr_ram) chr_ref(v) = value; break;
    // Nametables
    case 0x2000 ... 0x3EFF: write_nt(v, value); break;
    // Palettes
    case 0x3F00 ... 0x3FFF:
        {
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
}

uint8_t read_ppu_reg(unsigned n) {
    switch (n) {

    // Write-only registers
    case 0: case 1: case 3: case 5: case 6: return get_all_open_bus_bits();

    case 2:
        if (scanline == 241) {
            // Quirkiness related to reading $2002 around the point where the
            // VBlank flag is set. TODO: Elaborate on timing.
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
        ppu_open_bus    = (in_vblank << 7) | (sprite_zero_hit << 6) | (sprite_overflow << 5) |
                          get_open_bus_bits_4_to_0();
        in_vblank       = false;
        open_bus_bits_7_to_5_refreshed();
        return ppu_open_bus;

    case 4:
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

    case 7:
        {
        uint8_t const res = read_vram();
        do_2007_post_access_bump();
        return res;
        }

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

    // PPUCTRL
    case 0:
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

    // PPUMASK
    case 1:
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

    // PPUSTATUS
    case 2: break;

    // OAMADDR
    case 3: oam_addr = value; break;

    // OAMDATA
    case 4: write_oam_data_reg(value); break;

    // PPUSCROLL
    case 5:
        if (initial_frame) {
            printf("Warning: Writing PPUSCROLL during initial frame, at (%u,%u)\n", scanline, dot);
            return;
        }

        if (!write_flip_flop) {
            // First write
            // fine_x = value: .... .ABC
            // t: ... .... ...D EFGH = value: DEFG H...
            fine_x = value & 7;
            t      = (t & 0x7FE0) | ((value & 0xF8) >> 3);
        }
        else
            // Second write
            // t: ABC ..DE FGH. .... = value: DEFG HABC
            t = (t & 0x0C1F) | ((value & 0xF8) << 2) | ((value & 7) << 12);

        write_flip_flop = !write_flip_flop;
        break;

    // PPUADDR
    case 6:
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

    // PPUDATA
    case 7:
        write_vram(value);
        do_2007_post_access_bump();
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
