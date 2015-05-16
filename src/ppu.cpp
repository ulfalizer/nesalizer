#include "common.h"

#include "cpu.h"
#include "ppu.h"
#include "mapper.h"
#include "rom.h"
#include "sdl_backend.h"
#include "timing.h"

#include "palette.inc"

// Points to the current palette as determined by the color tint bits
static uint32_t const     *pal_to_rgb;

// If true, treat the emulated code as the first code that runs (i.e., not the
// situation on PowerPak), which means writes to certain registers will be
// inhibited during the initial frame. This breaks some demos.
bool const                starts_on_initial_frame = false;

uint8_t                   *ciram;

unsigned                  prerender_line;

static uint8_t            palettes[0x20];
static uint8_t            oam[0x100];
static uint8_t            sec_oam[0x20];

// VRAM address/scroll regs. 15 bits long.
static unsigned           t, v;
static uint8_t            fine_x;
// v is not immediately updated from t on the second write to $2006. This
// variable implements the delay.
static unsigned           pending_v_update;

static unsigned           v_inc;           // $2000:2
static uint16_t           sprite_pat_addr; // $2000:3
static uint16_t           bg_pat_addr;     // $2000:4
static enum Sprite_size {
    EIGHT_BY_EIGHT,
    EIGHT_BY_SIXTEEN
}                         sprite_size;   // $2000:5
static bool               nmi_on_vblank; // $2000:7

// $2001:0 - 0x30 if grayscale mode enabled, otherwise 0x3F
static uint8_t            grayscale_color_mask;
static bool               show_bg_left_8;       // $2001:1
static bool               show_sprites_left_8;  // $2001:2
static bool               show_bg;              // $2001:3
static bool               show_sprites;         // $2001:4
static uint8_t            tint_bits;            // $2001:7-5

bool                      rendering_enabled;
// Optimizations - if bg/sprites are disabled, a value is set that causes
// comparisons to always fail. If the leftmost 8 pixels should be clipped,
// comparisons only fail for those pixels. Otherwise, comparisons never fail.
static unsigned           bg_clip_comp;
static unsigned           sprite_clip_comp;

static bool               sprite_overflow; // $2002:5
static bool               sprite_zero_hit; // $2002:6
static bool               in_vblank;       // $2002:7

static uint8_t            oam_addr; // $2003
// Pointer into the secondary OAM, 5 bits wide
//  - Updated during sprite evaluation and loading
//  - Cleared at dots 64.5, 256.5 and 340.5, if rendering
static unsigned           sec_oam_addr;
static uint8_t            oam_data; // $2004 (seen when reading from $2004)

// Sprite evaluation state

// Goes high for three ticks when an in-range sprite is found during sprite
// evaluation
static unsigned           copy_sprite_signal;
static bool               oam_addr_overflow, sec_oam_addr_overflow;
static bool               overflow_detection;

// PPUSCROLL/PPUADDR write flip-flop. First write when false, second write when
// true.
static bool               write_flip_flop;

static uint8_t            ppu_data_reg; // $2007 read buffer

static bool               odd_frame;

uint64_t                  ppu_cycle;

// Internal PPU counters and registers

unsigned                  dot, scanline;

static uint8_t            nt_byte, at_byte;
static uint8_t            bg_byte_l, bg_byte_h;
static uint16_t           bg_shift_l, bg_shift_h;
static unsigned           at_shift_l, at_shift_h;
static unsigned           at_latch_l, at_latch_h;

static uint8_t            sprite_attribs[8];
static uint8_t            sprite_x[8];
static uint8_t            sprite_pat_l[8];
static uint8_t            sprite_pat_h[8];

static bool               s0_on_next_scanline;
static bool               s0_on_cur_scanline;

// Temporary storage (also exists in PPU) for data during sprite loading
static uint8_t            sprite_y, sprite_index;
static bool               sprite_in_range;

// Writes to certain registers are suppressed during the initial frame:
// http://wiki.nesdev.com/w/index.php/PPU_power_up_state
//
// Emulating this makes NY2011 and possibly other demos hang. They probably
// don't run on the real thing either.
static bool               initial_frame;

unsigned                  ppu_addr_bus;

// Open bus for reads from PPU $2000-$2007 (tested by ppu_open_bus.nes).
// "wcycle" is short for "write cycle".

static uint8_t            ppu_open_bus;
static uint64_t           bit_7_6_wcycle, bit_5_wcycle, bit_4_0_wcycle;

static unsigned           open_bus_decay_cycles;

void init_ppu_for_rom() {
    prerender_line = is_pal ? 311 : 261;
    // PPU open bus values fade after about 600 ms
    open_bus_decay_cycles = 0.6*ppu_clock_rate;
}

static void open_bus_refreshed() {
    bit_7_6_wcycle = bit_5_wcycle = bit_4_0_wcycle = ppu_cycle;
}

static void open_bus_bits_7_to_5_refreshed() {
    bit_7_6_wcycle = bit_5_wcycle = ppu_cycle;
}

static void open_bus_bits_5_to_0_refreshed() {
    bit_5_wcycle = bit_4_0_wcycle = ppu_cycle;
}

static uint8_t get_open_bus_bits_7_to_6() {
    return (ppu_cycle - bit_7_6_wcycle > open_bus_decay_cycles) ?
             0 : ppu_open_bus & 0xC0;
}

static uint8_t get_open_bus_bits_4_to_0() {
    return (ppu_cycle - bit_4_0_wcycle > open_bus_decay_cycles) ?
             0 : ppu_open_bus & 0x1F;
}

static uint8_t get_all_open_bus_bits() {
    return get_open_bus_bits_7_to_6() |
           ((ppu_cycle - bit_5_wcycle > open_bus_decay_cycles) ?
             0 : ppu_open_bus & 0x20) |
           get_open_bus_bits_4_to_0();
}

static uint8_t &chr_ref(unsigned chr_addr) {
    return chr_pages[(chr_addr >> 10) & 7][chr_addr & 0x03FF];
}

// Nametable reading and writing

// Returns the physical CIRAM address after mirroring
static uint16_t get_mirrored_addr(uint16_t addr) {
    switch (mirroring) {
    case VERTICAL:        return addr & 0x07FF;
    case HORIZONTAL:      return ((addr >> 1) & 0x0400) + (addr & 0x03FF);
    case ONE_SCREEN_LOW:  return addr & 0x03FF;
    case ONE_SCREEN_HIGH: return 0x0400 + (addr & 0x03FF);
    case FOUR_SCREEN:     return addr & 0x0FFF;
    default: UNREACHABLE;
    }
}

static uint8_t read_nt(uint16_t addr) {
    return mapper_fns.read_nt ?
             mapper_fns.read_nt(addr) :
             ciram[get_mirrored_addr(addr)];
}

static void write_nt(uint16_t addr, uint8_t val) {
    if (mapper_fns.write_nt)
        mapper_fns.write_nt(val, addr);
    else
        ciram[get_mirrored_addr(addr)] = val;
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
    // Fine y equal to 7?
    if ((v & 0x7000) == 0x7000)
        // Check coarse y
        switch (v & 0x03E0) {

        // Coarse y equal to 29. Switch vertical nametable (XOR by 0x0800) and
        // clear fine y and coarse y in the same operation (possible since we
        // know their value).
        case 29 << 5: v ^= 0x7800 | (29 << 5); break;

        // Coarse y equal to 31. Clear fine y and coarse y without switching
        // vertical nametable (this occurs for vertical scroll values > 240).
        case 31 << 5: v &= ~0x73E0; break;

        // Clear fine y and increment coarse y
        default: v = (v & ~0x7000) + 0x0020;
        }
    else
        // Bump fine y
        v += 0x1000;
}

// Restores the horizontal bits in v from t at the end of each scanline during
// rendering
static void copy_horiz() {
    // v: ... .H.. ...E DCBA = t: ... .H.. ...E DCBA
    v = (v & ~0x041F) | (t & 0x041F);
}

// Initializes the vertical bits in v from t on the pre-render line
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

// Looks for an in-range sprite pixel at the current location.
// Performance hotspot!
// Possible optimization: Set flag if any sprites on the line
static unsigned get_sprite_pixel(unsigned &spr_pal, bool &spr_behind_bg, bool &spr_is_s0) {
    unsigned const pixel = dot - 2;
    // Equivalent to 'if (!show_sprites || (!show_sprites_left_8 && pixel < 8))'
    if (pixel < sprite_clip_comp)
        return 0;

    for (unsigned i = 0; i < 8; ++i) {
        unsigned const offset = pixel - sprite_x[i];
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
static void do_pixel_output_and_sprite_zero() {
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
        // unsigned const at_bits =
        //   at_byte >> 2*((coarse_y & 0x02) | (((coarse_x - 1) & 0x02) >> 1));
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
        return;
    }

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
        // We're currently copying data for a sprite
        --copy_sprite_signal;
        move_to_next_oam_byte();
        return;
    }

    // Is the current sprite in range?
    bool const in_range = (scanline - orig_oam_data) < (sprite_size == EIGHT_BY_EIGHT ? 8 : 16);
    // At dot 66 we're evaluating sprite zero. This is how the hardware does it.
    if (dot == 66)
        s0_on_next_scanline = in_range;

    if (in_range && !(oam_addr_overflow || sec_oam_addr_overflow)) {
        // In-range sprite found. Copy it.
        copy_sprite_signal = 3;
        move_to_next_oam_byte();
        return;
    }

    // Sprite is not in range (or we have OAM or secondary OAM overflow)

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

// Returns 'true' if the sprite is in range
static bool calc_sprite_tile_addr(uint8_t y, uint8_t index, uint8_t attrib, bool is_high) {
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
    unsigned const sprite_n = (dot - 257)/8;

    if (dot == 257)
        sec_oam_addr = 0;

    // Sprite 0 flag timing:
    //  - s0_on_next_scanline is initialized at dot = 66.5-67 (during sprite
    //    evaluation for sprite 0)
    //  - It is copied over to s0_on_cur_scanline during dots
    //    257.5-258, 258.5-259, ..., 319.5-320
    s0_on_cur_scanline = s0_on_next_scanline;

    switch ((dot - 1) % 8) {

    // Load sprite attributes from secondary OAM

    case 0:
        // TODO: How does the sprite_y/index loading work in detail?

        // Dummy NT fetch
        ppu_addr_bus = 0x2000 | (v & 0x0FFF);

        sprite_y = sec_oam[sec_oam_addr];
        sec_oam_addr = (sec_oam_addr + 1) & 0x1F;
        break;
    case 1:
        sprite_index = sec_oam[sec_oam_addr];
        sec_oam_addr = (sec_oam_addr + 1) & 0x1F;
        break;
    case 2:
        // Dummy "AT" fetch, which is actually an NT fetch too
        ppu_addr_bus = 0x2000 | (v & 0x0FFF);

        sprite_attribs[sprite_n] = sec_oam[sec_oam_addr];
        sec_oam_addr = (sec_oam_addr + 1) & 0x1F;
        break;
    case 3:
        sprite_x[sprite_n] = sec_oam[sec_oam_addr];
        sec_oam_addr = (sec_oam_addr + 1) & 0x1F;
        break;

    // Load low sprite tile byte

    case 4:
        sprite_in_range =
          calc_sprite_tile_addr(sprite_y, sprite_index, sprite_attribs[sprite_n], false);
        break;
    case 5:
        sprite_pat_l[sprite_n] = sprite_in_range ? chr_ref(ppu_addr_bus) : 0;
        // Horizontal flipping
        if (sprite_attribs[sprite_n] & 0x40)
            sprite_pat_l[sprite_n] = rev_byte(sprite_pat_l[sprite_n]);
        break;

    // Load high sprite tile byte

    case 6:
        sprite_in_range =
          calc_sprite_tile_addr(sprite_y, sprite_index, sprite_attribs[sprite_n], true);
        break;
    case 7:
        sprite_pat_h[sprite_n] = sprite_in_range ? chr_ref(ppu_addr_bus) : 0;
        // Horizontal flipping
        if (sprite_attribs[sprite_n] & 0x40)
            sprite_pat_h[sprite_n] = rev_byte(sprite_pat_h[sprite_n]);
        break;

    default: UNREACHABLE
    }
}

// Common operations for the visible lines (0-239) and the pre-render line.
// Performance hotspot!
static void do_render_line_ops() {
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

// Called for dots on the visible lines (0-239)
static void do_visible_line_ops() {
    if (dot >= 2 && dot <= 257)
        do_pixel_output_and_sprite_zero();

    if (rendering_enabled) {
        do_render_line_ops();

        switch (dot) {
        case 1 ... 64:
            // Secondary OAM clear
            if (dot & 1)
                oam_data = 0xFF;
            else {
                sec_oam[sec_oam_addr] = oam_data;
                // Should this be done when setting oam_data? Extremely
                // obscure.
                sec_oam_addr = (sec_oam_addr + 1) & 0x1F;
            }
            break;

        case 65 ... 256:
            do_sprite_evaluation();
        }
    }
}

// Called for dots on line 241
static void do_line_241_ops() {
    if (dot == 1) {
        in_vblank = true;
        set_nmi(nmi_on_vblank);
    }
}

// Called for dots on the pre-render line
static void do_prerender_line_ops() {
    // This might be one tick off due to the possibility of reading the flags
    // really shortly after they are cleared in the preferred alignment
    if (dot == 1) sprite_overflow = sprite_zero_hit = initial_frame = false;
    // TODO: Explain why the timing works out like this (and is it cycle-perfect?)
    if (dot == 2) in_vblank = false;

    if (rendering_enabled) {
        do_render_line_ops();

        // This is where s0_on_next_scanline is initialized on the
        // prerender line the hardware. There's an "in visible frame"
        // condition on the value the flag is initialized to - hence it
        // always becomes false.
        if (dot == 66)
            s0_on_next_scanline = false;

        if (dot >= 280 && dot <= 304)
            copy_vert();
    }
}

// Runs the PPU for one dot.
// Performance hotspot - ticks at ~5.3 MHz
//
// IS_PAL is set true for PAL emulation, with PRERENDER_LINE set accordingly to
// the scanline number of the pre-render line (the final line of the frame).
// These are also available as 'is_pal' and 'prerender_line', but kept as
// compile-time constants here for performance.
template<bool IS_PAL, unsigned PRERENDER_LINE>
static void tick_ppu() {
    ++ppu_cycle;

    // Move to next tick - doing this first mirrors how Visual 2C02 views it
    if (++dot == 341) {
        dot = 0;
        ++scanline;
        // Possible optimization: set an enum indicating the scanline range
        // here and use below (SCANLINE_0_TO_239, SCANLINE_241, etc.)
        switch (scanline) {
        case 240:
            frame_completed();
            // The PPU address bus mirrors v outside of rendering
            ppu_addr_bus = v & 0x3FFF;
            break;

        case PRERENDER_LINE + 1:
            scanline = 0;
            if (!IS_PAL) {
                if (rendering_enabled && odd_frame) ++dot;
                odd_frame = !odd_frame;
            }
        }
    }

    if (pending_v_update > 0 && --pending_v_update == 0) {
        v = t;
        if ((scanline >= 240 && scanline < PRERENDER_LINE) || !rendering_enabled)
            // The PPU address bus mirrors v outside of rendering
            ppu_addr_bus = v & 0x3FFF;
    }

    switch (scanline) {
    case 0 ... 239     : do_visible_line_ops();   break;
    case 241           : do_line_241_ops();       break;
    case PRERENDER_LINE: do_prerender_line_ops();
    }

    // Mapper-specific operations - usually to snoop on ppu_addr_bus
    mapper_fns.ppu_tick_callback();
}

void tick_ntsc_ppu() {
    tick_ppu<false, 261>();
}

void tick_pal_ppu() {
    tick_ppu<true, 311>();
}

static void do_2007_post_access_bump() {
    if (rendering_enabled && (scanline < 240 || scanline == prerender_line)) {
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

static void write_vram(uint8_t val) {
    switch (v & 0x3FFF) {

    // Pattern tables
    case 0x0000 ... 0x1FFF: if (chr_is_ram) chr_ref(v) = val; break;
    // Nametables
    case 0x2000 ... 0x3EFF: write_nt(v, val); break;
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

        palettes[palette_write_mirror[v & 0x1F]] = palettes[v & 0x1F] = val & 0x3F;
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
                in_vblank = false;
                set_nmi(false);
                break;

            case 2: case 3:
                set_nmi(false);
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
        {
        // Micro machines reads this during rendering
        if (rendering_enabled && (scanline < 240 || scanline == prerender_line)) {
            // TODO: Make this work automagically through proper emulation of
            // the interval after the sprite fetches
            if (dot >= 323)
                return sec_oam[0];
            return oam_data;
        }
        open_bus_refreshed();

        // Some of the attribute bits do not exist and always read back as zero
        static uint8_t const mask_lut[] = { 0xFF, 0xFF, 0xE3, 0xFF };
        return ppu_open_bus = oam[oam_addr] & mask_lut[oam_addr & 3];
        }

    case 7:
        {
        uint8_t const res = read_vram();
        do_2007_post_access_bump();
        return res;
        }

    default: UNREACHABLE
    }
}

void write_oam_data_reg(uint8_t val) {
    // OAM updates are inhibited during rendering. $2004 writes during
    // rendering do perform a glitchy oam_addr increment however, but that
    // might be hard to pin down (could depend on current sprite evaluation
    // status for example) and not worth emulating.
    if (rendering_enabled && (scanline < 240 || scanline == prerender_line))
        return;
    oam[oam_addr++] = val;
}

static void set_derived_ppumask_vars() {
    rendering_enabled = show_bg || show_sprites;
    bg_clip_comp      = !show_bg      ? 256 : show_bg_left_8      ? 0 : 8;
    sprite_clip_comp  = !show_sprites ? 256 : show_sprites_left_8 ? 0 : 8;
    // The status of the tint bits determines the current palette
    pal_to_rgb        = nes_to_rgb[tint_bits];
}

void write_ppu_reg(uint8_t val, unsigned n) {
    ppu_open_bus = val;
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
        t               = (t & 0x73FF) | ((val & 0x03) << 10);
        v_inc           = (val & 0x04) ? 32 : 1;
        sprite_pat_addr = (val & 0x08) << 9; // val & 0x08 ? 0x1000 : 0x0000
        bg_pat_addr     = (val & 0x10) << 8; // val & 0x10 ? 0x1000 : 0x0000
        sprite_size     = val & 0x20 ? EIGHT_BY_SIXTEEN : EIGHT_BY_EIGHT;

        bool const new_nmi_on_vblank = val & 0x80;
        if (new_nmi_on_vblank) {
            // An unset-to-set transition in nmi_on_vblank while in_vblank is
            // set causes another NMI to be generated, since the NMI line
            // equals nmi_on_vblank AND in_vblank (though it's active low
            // instead): http://wiki.nesdev.com/w/index.php/NMI
            if (!nmi_on_vblank && in_vblank)
                set_nmi(true);
        }
        else
            // This ensures that no NMI is generated if NMIs are disabled right
            // around where the vblank flag is set. We might get a short NMI
            // pulse in that case, but it won't be seen.
            set_nmi(false);

        nmi_on_vblank = new_nmi_on_vblank;
        break;
        }

    // PPUMASK
    case 1:
        if (initial_frame) {
            printf("Warning: Writing PPUMASK during initial frame, at (%u,%u)\n", scanline, dot);
            return;
        }

        grayscale_color_mask = val & 0x01 ? 0x30 : 0x3F;
        show_bg_left_8       = val & 0x02;
        show_sprites_left_8  = val & 0x04;
        show_bg              = val & 0x08;
        show_sprites         = val & 0x10;
        tint_bits            = (val >> 5) & 7;

        set_derived_ppumask_vars();

        break;

    // PPUSTATUS
    case 2: break;

    // OAMADDR
    case 3: oam_addr = val; break;

    // OAMDATA
    case 4: write_oam_data_reg(val); break;

    // PPUSCROLL
    case 5:
        if (initial_frame) {
            printf("Warning: Writing PPUSCROLL during initial frame, at (%u,%u)\n", scanline, dot);
            return;
        }

        if (!write_flip_flop) {
            // First write
            // fine_x = val: .... .ABC
            // t: ... .... ...D EFGH = val: DEFG H...
            fine_x = val & 7;
            t      = (t & 0x7FE0) | ((val & 0xF8) >> 3);
        }
        else
            // Second write
            // t: ABC ..DE FGH. .... = val: DEFG HABC
            t = (t & 0x0C1F) | ((val & 0xF8) << 2) | ((val & 7) << 12);

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
            // t: 0AB CDEF .... .... = val: ..AB CDEF
            // Clearing of high bit confirmed in Visual 2C02
            t = (t & 0x00FF) | ((val & 0x3F) << 8);
        else {
            // Second write
            // t: ... .... ABCD EFGH = val: ABCD EFGH
            t = (t & 0x7F00) | val;
            // There is a delay of ~3 ticks before t is copied to v
            pending_v_update = 3;
        }

        write_flip_flop = !write_flip_flop;
        break;

    // PPUDATA
    case 7:
        write_vram(val);
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
    bit_7_6_wcycle = bit_5_wcycle = bit_4_0_wcycle = 0;

    // Render pipeline buffers and shift registers and sprite output units

    nt_byte    = at_byte    = 0;
    bg_byte_l  = bg_byte_h  = 0;
    bg_shift_l = bg_shift_h = 0;
    at_shift_l = at_shift_h = 0;
    at_latch_l = at_latch_h = 0;

    sprite_y = sprite_index = 0;
    sprite_in_range = false;

    init_array(sprite_attribs, (uint8_t)0);
    init_array(sprite_x      , (uint8_t)0);
    init_array(sprite_pat_l  , (uint8_t)0);
    init_array(sprite_pat_h  , (uint8_t)0);
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
    if (chr_is_ram) TRANSFER_P(chr_base, chr_8k_banks*0x2000);
    TRANSFER_P(ciram, mirroring == FOUR_SCREEN ? 0x1000 : 0x800);
    TRANSFER(palettes)
    TRANSFER(oam) TRANSFER(sec_oam)
    TRANSFER(t) TRANSFER(v) TRANSFER(fine_x)
    TRANSFER(pending_v_update)

    TRANSFER(v_inc)
    TRANSFER(sprite_pat_addr)
    TRANSFER(bg_pat_addr)
    TRANSFER(sprite_size)
    TRANSFER(nmi_on_vblank)

    TRANSFER(grayscale_color_mask)
    TRANSFER(show_bg_left_8)
    TRANSFER(show_sprites_left_8)
    TRANSFER(show_bg)
    TRANSFER(show_sprites)
    TRANSFER(tint_bits)
    if (!is_save)
        set_derived_ppumask_vars();

    TRANSFER(sprite_overflow) TRANSFER(sprite_zero_hit) TRANSFER(in_vblank)

    TRANSFER(oam_addr) TRANSFER(sec_oam_addr) TRANSFER(oam_data)

    TRANSFER(copy_sprite_signal)
    TRANSFER(oam_addr_overflow) TRANSFER(sec_oam_addr_overflow)
    TRANSFER(overflow_detection)

    TRANSFER(write_flip_flop)
    TRANSFER(ppu_data_reg)
    TRANSFER(odd_frame)
    TRANSFER(ppu_cycle)

    TRANSFER(dot) TRANSFER(scanline)

    TRANSFER(nt_byte) TRANSFER(at_byte)
    TRANSFER(bg_byte_l) TRANSFER(bg_byte_h)
    TRANSFER(bg_shift_l) TRANSFER(bg_shift_h)
    TRANSFER(at_shift_l) TRANSFER(at_shift_h)
    TRANSFER(at_latch_l) TRANSFER(at_latch_h)

    TRANSFER(sprite_attribs)
    TRANSFER(sprite_x)
    TRANSFER(sprite_pat_l)
    TRANSFER(sprite_pat_h)

    TRANSFER(s0_on_next_scanline)
    TRANSFER(s0_on_cur_scanline)

    TRANSFER(sprite_y) TRANSFER(sprite_index)
    TRANSFER(sprite_in_range)

    TRANSFER(initial_frame)

    TRANSFER(ppu_addr_bus)

    TRANSFER(ppu_open_bus)
    TRANSFER(bit_7_6_wcycle) TRANSFER(bit_5_wcycle) TRANSFER(bit_4_0_wcycle)
}

// Explicit instantiations

// Calculating state size
template void transfer_ppu_state<true, false>(uint8_t*&);
// Saving state to buffer
template void transfer_ppu_state<false, true>(uint8_t*&);
// Loading state from buffer
template void transfer_ppu_state<false, false>(uint8_t*&);
