// Mapper-2-ish

#include "common.h"

#include "mapper.h"
#include "rom.h"

// TODO: This mapper has variants that work differently

void mapper_71_init() {
    // Last PRG bank and all CHR banks fixed
    set_prg_16k_bank(1, prg_16k_banks - 1);
    set_chr_8k_bank(0);

    // Default
    set_prg_16k_bank(0, 0);
}

void mapper_71_write(uint8_t value, uint16_t addr) {
    if (!(~addr & 0xC000))
        // $C000-$FFFF
        set_prg_16k_bank(0, value);
}
