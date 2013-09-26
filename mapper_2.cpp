// Most common configuration of UxROM

#include "common.h"

#include "mapper.h"
#include "rom.h"

void mapper_2_init() {
    // Last PRG bank and all CHR banks fixed
    set_prg_16k_bank(1, prg_16k_banks - 1);
    set_chr_8k_bank(0);

    // Default
    set_prg_16k_bank(0, 0);
}

void mapper_2_write(uint8_t value, uint16_t) {
    set_prg_16k_bank(0, value);
}
