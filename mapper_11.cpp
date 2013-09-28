// Color Dreams

#include "common.h"

#include "mapper.h"

void mapper_11_init() {
    set_prg_32k_bank(0);
    set_chr_8k_bank(0);
}

void mapper_11_write(uint8_t value, uint16_t addr) {
    if (!(addr & 0x8000)) return;

    // PRG reg only has three bits, but supporting oversize probably won't hurt
    set_prg_32k_bank(value);
    set_chr_8k_bank(value >> 4);
}
