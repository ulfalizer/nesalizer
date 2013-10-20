// AxROM

#include "common.h"

#include "mapper.h"

static uint8_t reg;

static void apply_state() {
    set_mirroring(reg & 0x10 ? ONE_SCREEN_HIGH : ONE_SCREEN_LOW);
    set_prg_32k_bank(reg & 7);
}

void mapper_7_init() {
    set_chr_8k_bank(0); // CHR fixed
    reg = 0;
    apply_state();
}

void mapper_7_write(uint8_t value, uint16_t addr) {
    if (!(addr & 0x8000)) return;
    reg = value;
    apply_state();
}

MAPPER_STATE_START(7)
  MAPPER_STATE(reg)
MAPPER_STATE_END(7)
