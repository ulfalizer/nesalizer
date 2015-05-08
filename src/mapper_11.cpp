// Color Dreams

#include "common.h"

#include "mapper.h"

uint8_t prg_bank, chr_bank;

static void apply_state() {
    set_prg_32k_bank(prg_bank);
    set_chr_8k_bank(chr_bank);
}

void mapper_11_init() {
    prg_bank = chr_bank = 0;
    apply_state();
}

void mapper_11_write(uint8_t val, uint16_t addr) {
    if (!(addr & 0x8000)) return;
    prg_bank = val & 3;
    chr_bank = val >> 4;
    apply_state();
}

MAPPER_STATE_START(11)
  TRANSFER(prg_bank)
  TRANSFER(chr_bank)
MAPPER_STATE_END(11)
