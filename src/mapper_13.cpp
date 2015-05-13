// NES-CPROM - only used by Videomation

#include "common.h"

#include "mapper.h"

static uint8_t chr_bank;

static void apply_state() {
    set_chr_4k_bank(1, chr_bank);
}

void mapper_13_init() {
    // PRG and lower CHR bank fixed
    set_prg_32k_bank(0);
    set_chr_4k_bank(0, 0);

    // Guess
    chr_bank = 0;

    apply_state();
}

void mapper_13_write(uint8_t val, uint16_t addr) {
    if (!(addr & 0x8000)) return;
    chr_bank = val & 3;
    apply_state();
}

MAPPER_STATE_START(13)
  TRANSFER(chr_bank)
MAPPER_STATE_END(13)
