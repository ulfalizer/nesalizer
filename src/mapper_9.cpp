// MMC2 - only used by Punch-Out!!

#include "common.h"

#include "mapper.h"
#include "ppu.h"

static uint8_t prg_bank;

// Index 0 is from $B000/$D000, index 1 from $C000/$E000
static uint8_t chr_low_bank[2];
static uint8_t chr_high_bank[2];

static bool chr_low_uses_C000, chr_high_uses_E000;

// Assume the CHR switch-over happens when the PPU address bus goes from one of
// the magic values to some other value (maybe not perfectly accurate, but
// captures observed behavior)
static uint16_t prev_ppu_addr_bus;

static bool horizontal_mirroring;

static void apply_state() {
    set_prg_8k_bank(0, prg_bank);

    set_chr_4k_bank(0, chr_low_bank[chr_low_uses_C000]);
    set_chr_4k_bank(1, chr_high_bank[chr_high_uses_E000]);

    set_mirroring(horizontal_mirroring ? HORIZONTAL : VERTICAL);
}

void mapper_9_init() {
    // Last three 8 KB PRG banks fixed
    set_prg_8k_bank(1, -3);
    set_prg_8k_bank(2, -2);
    set_prg_8k_bank(3, -1);

    // Guess at defaults
    prg_bank = 0;
    chr_low_bank[0] = chr_low_bank[1] = 0;
    chr_high_bank[0] = chr_high_bank[1] = 0;
    chr_low_uses_C000 = chr_high_uses_E000 = false;
    prev_ppu_addr_bus = 0;

    apply_state();
}

void mapper_9_write(uint8_t val, uint16_t addr) {
    if (!(addr & 0x8000)) return;

    switch ((addr >> 12) & 7) {
    case 2: prg_bank             = val & 0x0F; break; // 0xA000
    case 3: chr_low_bank[0]      = val & 0x1F; break; // 0xB000
    case 4: chr_low_bank[1]      = val & 0x1F; break; // 0xC000
    case 5: chr_high_bank[0]     = val & 0x1F; break; // 0xD000
    case 6: chr_high_bank[1]     = val & 0x1F; break; // 0xE000
    case 7: horizontal_mirroring = val & 1;    break; // 0xF000
    }

    apply_state();
}

void mapper_9_ppu_tick_callback() {
    unsigned const magic_bits = ppu_addr_bus & 0x2FF8;

    if (magic_bits != 0x0FD8 && magic_bits != 0x0FE8) {
        // ppu_addr_bus is non-magic

        switch (prev_ppu_addr_bus) {
        case 0x0FD8:            chr_low_uses_C000  = false; apply_state(); break;
        case 0x0FE8:            chr_low_uses_C000  = true;  apply_state(); break;
        case 0x1FD8 ... 0x1FDF: chr_high_uses_E000 = false; apply_state(); break;
        case 0x1FE8 ... 0x1FEF: chr_high_uses_E000 = true;  apply_state(); break;
        }
    }

    prev_ppu_addr_bus = ppu_addr_bus;
}

MAPPER_STATE_START(9)
  TRANSFER(prg_bank)
  TRANSFER(chr_low_bank) TRANSFER(chr_high_bank)
  TRANSFER(chr_low_uses_C000) TRANSFER(chr_high_uses_E000)
  TRANSFER(prev_ppu_addr_bus)
  TRANSFER(horizontal_mirroring)
MAPPER_STATE_END(9)
