// AxROM

#include "common.h"

#include "mapper.h"

void mapper_7_init() {
    // CHR fixed
    set_chr_8k_bank(0);

    // Defaults
    set_mirroring(ONE_SCREEN_LOW);
    set_prg_32k_bank(0);
}

void mapper_7_write(uint8_t value, uint16_t) {
    set_mirroring(value & 0x10 ? ONE_SCREEN_HIGH : ONE_SCREEN_LOW);
    set_prg_32k_bank(value & 7);
}
