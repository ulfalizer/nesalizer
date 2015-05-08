// Most common configuration of UxROM

#include "common.h"

#include "mapper.h"

static uint8_t prg_bank;

static void apply_state() {
    set_prg_16k_bank(0, prg_bank);
}

void mapper_2_init() {
    // Last PRG bank and all CHR banks fixed
    set_prg_16k_bank(1, -1);
    set_chr_8k_bank(0);
    prg_bank = 0;
    apply_state();
}

void mapper_2_write(uint8_t val, uint16_t addr) {
    if (!(addr & 0x8000)) return;

    prg_bank = val;
    apply_state();
}

MAPPER_STATE_START(2)
  TRANSFER(prg_bank)
MAPPER_STATE_END(2)
