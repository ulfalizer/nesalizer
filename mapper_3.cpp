// CNROM board and a very similar board used for Panesian games

#include "common.h"

#include "mapper.h"

void mapper_3_init() {
    // No PRG swapping
    set_prg_32k_bank(0);
    // Default
    set_chr_8k_bank(0);
}

void mapper_3_write(uint8_t value, uint16_t addr) {
    if (!(addr & 0x8000)) return;

    // Actual reg is only 2 bits wide, but some homebrew ROMs (e.g.
    // lolicatgirls) assume more is possible
    set_chr_8k_bank(value);
}
