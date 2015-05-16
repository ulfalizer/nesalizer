// MMC5 - http://wiki.nesdev.com/w/index.php/INES_Mapper_005

#include "common.h"

#include "cpu.h"
#include "mapper.h"
#include "ppu.h"
#include "rom.h"

// 1 KB of extra on-chip memory
static uint8_t exram[1024];

// Mirroring:
//  ---------------------------
//    $5105:  [DDCC BBAA]
//
//
//  MMC5 allows each NT slot to be configured:
//    [   A   ][   B   ]
//    [   C   ][   D   ]
//
//  Values can be the following:
//    %00 = NES internal NTA
//    %01 = NES internal NTB
//    %10 = use ExRAM as NT
//    %11 = Fill Mode
//
//
//  For example... some typical mirroring setups would be:
//                (  D  C  B  A)
//    Horz:  $50  (%01 01 00 00)
//    Vert:  $44  (%01 00 01 00)
//    1ScA:  $00  (%00 00 00 00)
//    1ScB:  $55  (%01 01 01 01)
static uint8_t mmc5_mirroring;

// $5104:  [.... ..XX]    ExRAM mode
//     %00 = Extra Nametable mode    ("Ex0")
//     %01 = Extended Attribute mode ("Ex1")
//     %10 = CPU access mode         ("Ex2")
//     %11 = CPU read-only mode      ("Ex3")
static unsigned exram_mode;

static unsigned prg_mode;
static unsigned chr_mode;

static unsigned prg_banks[4];
static unsigned sprite_chr_banks[8];
static unsigned bg_chr_banks[4];

static unsigned wram_6000_bank;

static unsigned high_chr_bits; // $5130, pre-shifted by 6

// Built-in multiplier in $5205/$5206
static unsigned multiplicand, multiplier;

// Scanline IRQ and frame logic

static bool    irq_pending;
static bool    irq_enabled;
static uint8_t irq_scanline;
static uint8_t scanline_cnt;
static bool    in_frame;

// 'true' if the background CHR mappings are currently active. Only an
// optimization at the moment.
static bool using_bg_chr;

// Fill mode

static uint8_t fill_tile;
static uint8_t fill_attrib;

// Extended attribute mode

// Somehow the MMC5 "remembers" the previous non-attribute nametable fetch and
// is able to supply the corresponding attribute byte for the subsequent
// attribute fetch. Use this to keep track of the previously fetched
// non-attribute value from exram so we can do the same.
static uint8_t exram_val;

// Vertical split mode

// $5200
static bool     split_enabled;
static bool     split_on_right;
static unsigned split_tile_nr;
// $5201
static unsigned split_y_scroll;
// $5202
static unsigned split_chr_page;

static void use_bg_chr() {
    using_bg_chr = true;

    switch (chr_mode) {
    case 0:
        set_chr_8k_bank(bg_chr_banks[3]);
        break;

    case 1:
        set_chr_4k_bank(0, bg_chr_banks[3]);
        set_chr_4k_bank(1, bg_chr_banks[3]);
        break;

    case 2:
        set_chr_2k_bank(0, bg_chr_banks[1]);
        set_chr_2k_bank(1, bg_chr_banks[3]);
        set_chr_2k_bank(2, bg_chr_banks[1]);
        set_chr_2k_bank(3, bg_chr_banks[3]);
        break;

    case 3:
        set_chr_1k_bank(0, bg_chr_banks[0]);
        set_chr_1k_bank(1, bg_chr_banks[1]);
        set_chr_1k_bank(2, bg_chr_banks[2]);
        set_chr_1k_bank(3, bg_chr_banks[3]);
        set_chr_1k_bank(4, bg_chr_banks[0]);
        set_chr_1k_bank(5, bg_chr_banks[1]);
        set_chr_1k_bank(6, bg_chr_banks[2]);
        set_chr_1k_bank(7, bg_chr_banks[3]);
        break;

    default: UNREACHABLE
    }
}

static void use_sprite_chr() {
    using_bg_chr = false;

    switch (chr_mode) {
    case 0:
        set_chr_8k_bank(sprite_chr_banks[7]);
        break;

    case 1:
        set_chr_4k_bank(0, sprite_chr_banks[3]);
        set_chr_4k_bank(1, sprite_chr_banks[7]);
        break;

    case 2:
        set_chr_2k_bank(0, sprite_chr_banks[1]);
        set_chr_2k_bank(1, sprite_chr_banks[3]);
        set_chr_2k_bank(2, sprite_chr_banks[5]);
        set_chr_2k_bank(3, sprite_chr_banks[7]);
        break;

    case 3:
        for (unsigned n = 0; n < 8; ++n)
            set_chr_1k_bank(n, sprite_chr_banks[n]);
        break;

    default: UNREACHABLE
    }
}

static void apply_state() {
    switch (prg_mode) {
    case 0:
        set_prg_32k_bank(prg_banks[3] >> 2);
        break;

    case 1:
        set_prg_16k_bank(0, (prg_banks[1] & 0x7F) >> 1, !(prg_banks[1] & 0x80));
        set_prg_16k_bank(1, prg_banks[3] >> 1);
        break;

    case 2:
        set_prg_16k_bank(0, (prg_banks[1] & 0x7F) >> 1, !(prg_banks[1] & 0x80));
        set_prg_8k_bank(2, prg_banks[2] & 0x7F, !(prg_banks[2] & 0x80));
        set_prg_8k_bank(3, prg_banks[3]);
        break;

    case 3:
        set_prg_8k_bank(0, prg_banks[0] & 0x7F, !(prg_banks[0] & 0x80));
        set_prg_8k_bank(1, prg_banks[1] & 0x7F, !(prg_banks[1] & 0x80));
        set_prg_8k_bank(2, prg_banks[2] & 0x7F, !(prg_banks[2] & 0x80));
        set_prg_8k_bank(3, prg_banks[3]);
        break;

    default: UNREACHABLE
    }

    set_wram_6000_bank(wram_6000_bank);

    // Update the currently active CHR mapping
    if (using_bg_chr) {
        // The BG CHR bank registers are not used in extended attribute mode
        if (exram_mode != 1)
            use_bg_chr();
    }
    else
        use_sprite_chr();
}

void mapper_5_init() {
    init_array(exram, (uint8_t)0xFF);
    init_array(prg_banks, 0x7Fu);
    init_array(sprite_chr_banks, 0xFFu);
    init_array(bg_chr_banks, 0xFFu);

    prg_mode       = chr_mode = 3;
    wram_6000_bank = 7;
    mmc5_mirroring = 0xFF;
    high_chr_bits  = 0;
    multiplicand   = multiplier = 0;

    irq_pending = irq_enabled = in_frame = false;

    fill_tile = fill_attrib = 0;

    // Assume the sprite CHR banks are used at startup
    using_bg_chr = false;

    apply_state();
}

uint8_t mapper_5_read(uint16_t addr) {
    switch (addr) {
    case 0x5204:
    {
        uint8_t const res =
          (irq_pending  << 7) |
          (in_frame     << 6) |
          (cpu_data_bus & 0x3F);
        set_cart_irq((irq_pending = false));
        return res;
    }

    case 0x5205: return /*(uint8_t)*/ multiplicand * multiplier;
    case 0x5206: return /*(uint8_t)*/ (multiplicand * multiplier) >> 8;

    case 0x5C00 ... 0x5FFF:
        if (exram_mode == 2 || exram_mode == 3)
            return exram[addr - 0x5C00];
    }

    return cpu_data_bus; // Open bus
}

void mapper_5_write(uint8_t val, uint16_t addr) {
    if (addr < 0x5100) return;

    switch (addr) {
    case 0x5100: prg_mode = val & 3;     break;
    case 0x5101: chr_mode = val & 3;     break;
    case 0x5102: /* PRG RAM protect 1 */ break;
    case 0x5103: /* PRG RAM protect 2 */ break;
    case 0x5104: exram_mode = val & 3;   break;
    case 0x5105: mmc5_mirroring = val;   break;
    case 0x5106: fill_tile = val;        break;
    case 0x5107:
    {
        unsigned const attrib_bits = val & 3;
        fill_attrib = (attrib_bits << 6) | (attrib_bits << 4) | (attrib_bits << 2) | attrib_bits;
        break;
    }

    case 0x5113: wram_6000_bank = val & 7; break;

    case 0x5114 ... 0x5117:
        prg_banks[addr - 0x5114] = val;
        break;

    case 0x5120 ... 0x5127:
        sprite_chr_banks[addr - 0x5120] = high_chr_bits | val;
        break;

    case 0x5128 ... 0x512B:
        bg_chr_banks[addr - 0x5128] = high_chr_bits | val;
        break;

    case 0x5130: high_chr_bits = (val & 3) << 6; break;

    case 0x5200:
        split_enabled  = val & 0x80;
        split_on_right = val & 0x40;
        split_tile_nr  = val & 0x1F;
        break;
    case 0x5201: split_y_scroll = val; break;
    case 0x5202: split_chr_page = val; break;

    case 0x5203: irq_scanline = val; break;
    case 0x5204:
        irq_enabled = val & 0x80;
        set_cart_irq(irq_enabled && irq_pending);
        break;

    case 0x5205: multiplicand = val; break;
    case 0x5206: multiplier   = val; break;

    case 0x5C00 ... 0x5FFF:
        // In ExRAM modes 0 and 1, ExRAM is only writeable during rendering.
        // Outside of rendering, 0 gets written instead.
        switch (exram_mode) {
        case 0: case 1: exram[addr - 0x5C00] = in_frame ? val : 0; break;
        case 2:         exram[addr - 0x5C00] = val;                break;
        }
        break;
    }

    apply_state();
}

uint8_t mapper_5_read_nt(uint16_t addr) {
    if (exram_mode == 1) {
        // Extended attribute mode
        if (~addr & 0x3C0) {
            // Non-attribute nametable fetch. Fetch a byte from exram, switch
            // CHR banks according to its lower 6 bits, and remember it for the
            // following attribute byte fetch.
            unsigned const coarse_x = addr & 0x1F;
            unsigned const coarse_y = (addr >> 5) & 0x1F;
            exram_val = exram[32*coarse_y + coarse_x];
            unsigned const four_k_bank = high_chr_bits | (exram_val & 0x3F);
            // The bank gets mirrored across two 4 KB banks
            set_chr_4k_bank(0, four_k_bank);
            set_chr_4k_bank(1, four_k_bank);
        }
        else {
            // Use the remembered value from the previous non-attribute
            // nametable fetch. Duplicate the attribute bits in each of the
            // four attribute positions to make sure they get used regardless
            // of where we are in the nametable (might be what the real thing
            // does too).
            unsigned const attrib_bits = exram_val >> 6;
            return (attrib_bits << 6) | (attrib_bits << 4) | (attrib_bits << 2) | attrib_bits;
        }
    }

    // Vertical split mode can only be used in exram modes 0 and 1
    if (split_enabled && exram_mode <= 1) {
        // Assume the board is wired in CL mode
        // (http://wiki.nesdev.com/w/index.php/MMC5), meaning only the coarse
        // portion of the split's scroll value matters. This is true for the
        // only known game to use split screen mode (Uchuu Keibitai SDF).

        // The x coordinate of the tile on the screen. We need to account for
        // the first two tiles being pre-fetched at the end of the preceding
        // scanline (http://wiki.nesdev.com/w/images/4/4f/Ppu.svg).
        unsigned const tile_nr = (dot/8 + 2) % 40;

        if (( split_on_right && tile_nr >= split_tile_nr) ||
            (!split_on_right && tile_nr < split_tile_nr)) {
                // We're in the split area. The nametable data fetched is
                // determined purely by the screen position, which MMC5 keeps
                // track of; the coarse scroll we get from the address is
                // ignored.
                //
                // For reference, nametable addresses have the following
                // layout:
                //
                // y = fine y scroll
                // N = nametable select
                // Y = coarse Y scroll
                // X = coarse X scroll
                //   Non-attribute fetch: yyy NNYY YYYX XXXX
                //   Attribute fetch:      10 NN11 11<Y4><Y3> <Y2><X4><X3><X2>

                set_chr_4k_bank(0, split_chr_page);
                set_chr_4k_bank(1, split_chr_page);

                unsigned coarse_scroll = split_y_scroll >> 3;

                // If the split's scroll is set to less than 240 (or 30 for
                // when looking at the coarse scroll only), wrapping will skip
                // the attribute portion of the nametable. If it isn't, we wrap
                // at 256 (32) and interpret the initial attribute bytes as
                // tile indices. This works like ordinary nametables in the
                // PPU. The real MMC5 probably updates the scroll as it draws
                // the screen, meaning we might miss some odd corner cases
                // here. That'd be a PITA to emulate though, and isn't needed
                // for the only screen of the only game that uses this.
                unsigned coarse_y = (scanline/8 + coarse_scroll) % (coarse_scroll < 30 ? 30 : 32);

                if (dot & 2)
                    // Non-attribute fetch. Tile vs. attribute is probably
                    // position-based in the real MMC5, and determined by
                    // counting nametable accesses.
                    addr = ((coarse_y << 5) & 0x03E0) | tile_nr;
                else
                    // Attribute fetch
                    addr = 0x23C0 | ((coarse_y << 1) & 0x38) | (tile_nr >> 2);
                return exram[addr & 0x03FF];
        }
        else
            // Outside split area
            use_bg_chr();
    }

    // Maps $2000 to bits 1-0, $2400 to bits 3-2, etc.
    unsigned const bit_offset = (addr >> 9) & 6;
    switch ((mmc5_mirroring >> bit_offset) & 3) {
    // Internal nametable A
    case 0: return ciram[addr & 0x03FF];

    // Internal nametable B
    case 1: return ciram[0x0400 | (addr & 0x03FF)];

    // Use ExRAM as nametable
    case 2: return (exram_mode <= 1) ? exram[addr & 0x03FF] : 0;

    // Fill mode
    case 3:
        // If the nametable index is in the range 0x3C0-0x3FF, we're fetching
        // an attribute byte
        return (~addr & 0x3C0) ? fill_tile : fill_attrib;

    // Silences Clang warning
    default: UNREACHABLE
    }
}

void mapper_5_write_nt(uint8_t val, uint16_t addr) {
    // Maps $2000 to bits 1-0, $2400 to bits 3-2, etc.
    unsigned const bit_offset = (addr >> 9) & 6;
    switch ((mmc5_mirroring >> bit_offset) & 3) {
    // Internal nametable A
    case 0: ciram[addr & 0x03FF] = val; break;
    // Internal nametable B
    case 1: ciram[0x0400 | (addr & 0x03FF)] = val; break;
    // Use ExRAM as nametable
    case 2: if (exram_mode <= 1) exram[addr & 0x03FF] = val; break;
    // Assume the fill tile and attribute can't be written through the PPU in
    // mode 3
    }
}

void mapper_5_ppu_tick_callback() {
    // It is not known exactly how the MMC5 detects scanlines. Cheat by looking
    // at the current rendering position and status.

    if (!rendering_enabled || (scanline >= 240 && scanline != prerender_line)) {
        in_frame = false;
        // Uchuu Keibitai SDF reads nametable data from CHR and seems to expect
        // this.
        if (using_bg_chr)
            use_sprite_chr();
        return;
    }

    if (dot == 257)
        use_sprite_chr();
    else if (dot == 321)
        use_bg_chr();
    // 336 here shakes up Laser Invasion
    else if (dot == 337) {
        if (scanline < 240 || scanline == prerender_line) {
            if (!in_frame) {
                in_frame = true;
                scanline_cnt = 0;
                set_cart_irq((irq_pending = false));
            }
            else if (++scanline_cnt == irq_scanline) {
                irq_pending = true;
                if (irq_enabled)
                    set_cart_irq(true);
            }
        }
    }
}

MAPPER_STATE_START(5)
  TRANSFER(exram)
  TRANSFER(mmc5_mirroring)
  TRANSFER(exram_mode)
  TRANSFER(prg_mode)
  TRANSFER(chr_mode)
  TRANSFER(prg_banks)
  TRANSFER(sprite_chr_banks)
  TRANSFER(bg_chr_banks)
  TRANSFER(wram_6000_bank)
  TRANSFER(high_chr_bits)
  TRANSFER(multiplicand)
  TRANSFER(multiplier)
  TRANSFER(irq_pending)
  TRANSFER(irq_enabled)
  TRANSFER(irq_scanline)
  TRANSFER(scanline_cnt)
  TRANSFER(in_frame)
  TRANSFER(using_bg_chr)
  TRANSFER(fill_tile)
  TRANSFER(fill_attrib)
  TRANSFER(exram_val)
  TRANSFER(split_enabled)
  TRANSFER(split_on_right)
  TRANSFER(split_tile_nr)
  TRANSFER(split_y_scroll)
  TRANSFER(split_chr_page)
MAPPER_STATE_END(5)
