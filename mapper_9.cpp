// MMC2 - only used by Punch-Out!!

#include "common.h"

#include "mapper.h"
#include "ppu.h"

static unsigned chr_bank_0FDx, chr_bank_0FEx;
static unsigned chr_bank_1FDx, chr_bank_1FEx;

static bool low_bank_uses_0FDx, high_bank_uses_1FDx;

// Assume the CHR switch-over happens when the PPU address bus goes from one of
// the magic values to some other value (probably not perfectly accurate, but
// captures observed behavior)
static unsigned previous_magic_bits;

static bool horizontal_mirroring;

static void apply_state() {
    set_chr_4k_bank(0, low_bank_uses_0FDx  ? chr_bank_0FDx : chr_bank_0FEx);
    set_chr_4k_bank(1, high_bank_uses_1FDx ? chr_bank_1FDx : chr_bank_1FEx);

    set_mirroring(horizontal_mirroring ? HORIZONTAL : VERTICAL);
}

void mapper_9_init() {
    // Last three 8K PRG banks fixed
    set_prg_8k_bank(1, -3);
    set_prg_8k_bank(2, -2);
    set_prg_8k_bank(3, -1);

    // Guess at defaults
    set_prg_8k_bank(0, 0);
    set_chr_8k_bank(0);
    chr_bank_0FDx = chr_bank_0FEx = 0;
    chr_bank_1FDx = chr_bank_1FEx = 0;
    low_bank_uses_0FDx = high_bank_uses_1FDx = true;

    previous_magic_bits = 0;
}

void mapper_9_write(uint8_t value, uint16_t addr) {
    if (!(addr & 0x8000)) return;

    switch ((addr >> 12) & 7) {
    case 2: // 0xA000
        set_prg_8k_bank(0, value & 0x0F);
        break;

    case 3: // 0xB000
        chr_bank_0FDx = value & 0x1F;
        break;

    case 4: // 0xC000
        chr_bank_0FEx = value & 0x1F;
        break;

    case 5: // 0xD000
        chr_bank_1FDx = value & 0x1F;
        break;

    case 6: // 0xE000
        chr_bank_1FEx = value & 0x1F;
        break;

    case 7: // 0xF000
        horizontal_mirroring = value & 1;
        break;
    }

    apply_state();
}

static bool non_magic() {
    unsigned const magic_bits = ppu_addr_bus & 0xFFF0;
    return magic_bits != 0x0FD0 && magic_bits != 0x0FE0 &&
           magic_bits != 0x1FD0 && magic_bits != 0x1FE0;
}

void mapper_9_ppu_tick_callback() {
    // TODO: Optimize (beyond just moving stuff inside)
    switch (previous_magic_bits) {
    case 0x0FD0: if (non_magic()) low_bank_uses_0FDx  = true ; apply_state(); break;
    case 0x0FE0: if (non_magic()) low_bank_uses_0FDx  = false; apply_state(); break;
    case 0x1FD0: if (non_magic()) high_bank_uses_1FDx = true ; apply_state(); break;
    case 0x1FE0: if (non_magic()) high_bank_uses_1FDx = false; apply_state(); break;
    }

    previous_magic_bits = ppu_addr_bus & 0xFFF0;
}

MAPPER_STATE_START(9)
  MAPPER_STATE(chr_bank_0FDx) MAPPER_STATE(chr_bank_0FEx)
  MAPPER_STATE(chr_bank_1FDx) MAPPER_STATE(chr_bank_1FEx)
  MAPPER_STATE(low_bank_uses_0FDx) MAPPER_STATE(high_bank_uses_1FDx)
  MAPPER_STATE(previous_magic_bits)
  MAPPER_STATE(horizontal_mirroring)
MAPPER_STATE_END(9)
