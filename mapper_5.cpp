// MMC5 - http://wiki.nesdev.com/w/index.php/INES_Mapper_005

#include "common.h"

#include "cpu.h"
#include "mapper.h"
#include "ppu.h"
#include "sdl_backend.h"

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

static unsigned high_chr_bits; // $5130

// Built-in multiplier in $5205/$5206
static unsigned multiplicand, multiplier;

// Scanline IRQ and frame logic

static bool    irq_pending;
static bool    irq_enabled;
static uint8_t irq_scanline;
static uint8_t scanline_cnt;
static bool    in_frame;

// Fill mode

static uint8_t fill_tile;
static uint8_t fill_attrib;


static void switch_to_bg_chr() {
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

static void switch_to_sprite_chr() {
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

static void make_effective() {
    switch (prg_mode) {
    case 0:
        set_prg_32k_bank(prg_banks[3] >> 2);
        break;

    case 1:
        set_prg_16k_bank(0, (prg_banks[1] & 0x7F) >> 1);
        set_prg_16k_bank(1, prg_banks[3] >> 1);
        break;

    case 2:
        set_prg_16k_bank(0, (prg_banks[1] & 0x7F) >> 1);
        set_prg_8k_bank(2, prg_banks[2] & 0x7F);
        set_prg_8k_bank(3, prg_banks[3]);
        break;

    case 3:
        set_prg_8k_bank(0, prg_banks[0] & 0x7F);
        set_prg_8k_bank(1, prg_banks[1] & 0x7F);
        set_prg_8k_bank(2, prg_banks[2] & 0x7F);
        set_prg_8k_bank(3, prg_banks[3]);
        break;

    default: UNREACHABLE
    }

    if (dot <= 256 || dot >= 321)
        switch_to_bg_chr();
    else
        switch_to_sprite_chr();
}

void mapper_5_init() {
    init_array(exram, (uint8_t)0xFF);
    init_array(prg_banks, 0x7Fu);
    init_array(sprite_chr_banks, 0xFFu);
    init_array(bg_chr_banks, 0xFFu);

    prg_mode       = chr_mode = 3;
    mirroring      = SPECIAL;
    mmc5_mirroring = 0xFF;
    high_chr_bits  = 0;
    multiplicand   = multiplier = 0;

    irq_pending = irq_enabled = in_frame = false;

    fill_tile = fill_attrib = 0;

    make_effective();
}

uint8_t mapper_5_read(uint16_t addr) {
    switch (addr) {
    case 0x5204:
    {
        uint8_t const res =
          (irq_pending  << 7) |
          (in_frame     << 6) |
          (cpu_data_bus & 0x3F);
        cart_irq = irq_pending = false;
        update_irq_status();
        return res;
    }

    case 0x5205: return /*(uint8_t)*/ multiplicand * multiplier;
    case 0x5206: return /*(uint8_t)*/ (multiplicand * multiplier) >> 8;

    case 0x5C00 ... 0x5FFF:
        if (exram_mode == 2 || exram_mode == 3)
            return exram[addr - 0x5C00];
        return cpu_data_bus; // Open bus
    }
}

void mapper_5_write(uint8_t value, uint16_t addr) {
    if (addr < 0x5100) return;

    switch (addr) {
    case 0x5100: prg_mode = value & 3;   break;
    case 0x5101: chr_mode = value & 3;   break;
    case 0x5102: /* PRG RAM protect 1 */ break;
    case 0x5103: /* PRG RAM protect 2 */ break;
    case 0x5104: exram_mode = value & 3; break;
    case 0x5105: mmc5_mirroring = value; break;
    case 0x5106: fill_tile = value;      break;
    case 0x5107:
    {
        unsigned const attrib_bits = value & 3;
        fill_attrib = (attrib_bits << 6) | (attrib_bits << 4) | (attrib_bits << 2) | attrib_bits;
        LOG_MAPPER("Fill-mode attribute\n");
        break;
    }

    case 0x5113: /* PRG-RAM bankswitching */ break;

    case 0x5114 ... 0x5117:
        prg_banks[addr - 0x5114] = value;
        break;

    case 0x5120 ... 0x5127:
        sprite_chr_banks[addr - 0x5120] = value;
        break;

    case 0x5128 ... 0x512B:
        bg_chr_banks[addr - 0x5128] = value;
        break;

    case 0x5130: high_chr_bits = value & 3; break;

    case 0x5200: /* Vertical split mode   */ break;
    case 0x5201: /* Vertical split scroll */ break;
    case 0x5202: /* Vertical split bank   */ break;

    case 0x5203: irq_scanline = value; break;
    case 0x5204:
        irq_enabled = value & 0x80;
        cart_irq = irq_enabled && irq_pending;
        update_irq_status();
        break;

    case 0x5205: multiplicand = value; break;
    case 0x5206: multiplier   = value; break;

    case 0x5C00 ... 0x5FFF:
        // TODO: Only writeable during rendering for modes 0 and 1
        if (exram_mode != 3)
            exram[addr - 0x5C00] = value;
        break;
    }

    make_effective();
}

uint8_t mapper_5_read_nt(uint16_t addr) {
    unsigned bits;
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
    }
}

void mapper_5_write_nt(uint16_t addr, uint8_t value) {
    unsigned bits;
    // Maps $2000 to bits 1-0, $2400 to bits 3-2, etc.
    unsigned const bit_offset = (addr >> 9) & 6;
    switch ((mmc5_mirroring >> bit_offset) & 3) {
    // Internal nametable A
    case 0: ciram[addr & 0x03FF]            = value; break;
    // Internal nametable B
    case 1: ciram[0x0400 | (addr & 0x03FF)] = value; break;
    // Use ExRAM as nametable
    case 2: if (exram_mode <= 1) exram[addr & 0x03FF] = value;
    // Assume the fill tile and attribute can't be written through the PPU in
    // mode 3
    }
}

void mmc5_ppu_tick_callback() {
    // It is not known exactly how the MMC5 detects scanlines, so cheat by
    // looking at the current rendering position

    if (!rendering_enabled)
        in_frame = false;

    if (dot == 257)
        switch_to_sprite_chr();
    else if (dot == 321)
        switch_to_bg_chr();
    // 336 here shakes up Laser Invasion
    else if (dot == 337) {
        switch (scanline) {
        case 0 ... 239: case 261:
            if (!in_frame) {
                in_frame = true;
                scanline_cnt = 0;
                cart_irq = irq_pending = false;
                update_irq_status();
            }
            else if (++scanline_cnt == irq_scanline) {
                irq_pending = true;
                if (irq_enabled) {
                    cart_irq = true;
                    update_irq_status();
                }
            }

            if (scanline == 239)
                in_frame = false;

            break;
        }
    }
}
