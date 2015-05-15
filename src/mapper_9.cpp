// MMC2 - only used by Punch-Out!!

#include "common.h"

#include "mapper.h"
#include "ppu.h"

static uint8_t prg_bank;

static unsigned chr_bank_0FDx, chr_bank_0FEx;
static unsigned chr_bank_1FDx, chr_bank_1FEx;

static bool low_bank_uses_0FDx, high_bank_uses_1FDx;

// Assume the CHR switch-over happens when the PPU address bus goes from one of
// the magic values to some other value (maybe not perfectly accurate, but
// captures observed behavior)
static uint16_t prev_ppu_addr_bus;

static bool horizontal_mirroring;

static void apply_state() {
    set_prg_8k_bank(0, prg_bank);

    set_chr_4k_bank(0, low_bank_uses_0FDx  ? chr_bank_0FDx : chr_bank_0FEx);
    set_chr_4k_bank(1, high_bank_uses_1FDx ? chr_bank_1FDx : chr_bank_1FEx);

    set_mirroring(horizontal_mirroring ? HORIZONTAL : VERTICAL);
}

void mapper_9_init() {
    // Last three 8 KB PRG banks fixed
    set_prg_8k_bank(1, -3);
    set_prg_8k_bank(2, -2);
    set_prg_8k_bank(3, -1);

    // Guess at defaults
    prg_bank = 0;
    chr_bank_0FDx = chr_bank_0FEx = 0;
    chr_bank_1FDx = chr_bank_1FEx = 0;
    low_bank_uses_0FDx = high_bank_uses_1FDx = true;
    prev_ppu_addr_bus = 0;

    apply_state();
}

void mapper_9_write(uint8_t val, uint16_t addr) {
    if (!(addr & 0x8000)) return;

    switch ((addr >> 12) & 7) {
    case 2: prg_bank = val & 0x0F;          break; // 0xA000
    case 3: chr_bank_0FDx = val & 0x1F;     break; // 0xB000
    case 4: chr_bank_0FEx = val & 0x1F;     break; // 0xC000
    case 5: chr_bank_1FDx = val & 0x1F;     break; // 0xD000
    case 6: chr_bank_1FEx = val & 0x1F;     break; // 0xE000
    case 7: horizontal_mirroring = val & 1; break; // 0xF000
    }

    apply_state();
}

void mapper_9_ppu_tick_callback() {
    unsigned const magic_bits = ppu_addr_bus & 0x2FF8;

    if (magic_bits != 0x0FD8 && magic_bits != 0x0FE8) {
        // ppu_addr_bus is non-magic

        switch (prev_ppu_addr_bus) {
        case 0x0FD8:            low_bank_uses_0FDx  = true ; apply_state(); break;
        case 0x0FE8:            low_bank_uses_0FDx  = false; apply_state(); break;
        case 0x1FD8 ... 0x1FDF: high_bank_uses_1FDx = true ; apply_state(); break;
        case 0x1FE8 ... 0x1FEF: high_bank_uses_1FDx = false; apply_state(); break;
        }
    }

    prev_ppu_addr_bus = ppu_addr_bus;
}

MAPPER_STATE_START(9)
  TRANSFER(prg_bank)
  TRANSFER(chr_bank_0FDx) TRANSFER(chr_bank_0FEx)
  TRANSFER(chr_bank_1FDx) TRANSFER(chr_bank_1FEx)
  TRANSFER(low_bank_uses_0FDx) TRANSFER(high_bank_uses_1FDx)
  TRANSFER(prev_ppu_addr_bus)
  TRANSFER(horizontal_mirroring)
MAPPER_STATE_END(9)
