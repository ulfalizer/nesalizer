// Mapper-2-ish

#include "common.h"

#include "mapper.h"

// TODO: This mapper has variants that work differently

static uint8_t prg_bank;

static void apply_state() {
    set_prg_16k_bank(0, prg_bank);
}

void mapper_71_init() {
    // Last PRG bank and all CHR banks fixed
    set_prg_16k_bank(1, -1);
    set_chr_8k_bank(0);

    prg_bank = 0;
    apply_state();
}

void mapper_71_write(uint8_t val, uint16_t addr) {
    if (!(~addr & 0xC000)) {
        // $C000-$FFFF
        prg_bank = val;
        apply_state();
    }
}

MAPPER_STATE_START(71)
  TRANSFER(prg_bank)
MAPPER_STATE_END(71)
