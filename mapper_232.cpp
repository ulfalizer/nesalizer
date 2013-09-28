// Camerica/Capcom mapper, used by the Quattro * games

// Assume http://wiki.nesdev.com/w/index.php/Talk:INES_Mapper_232 is the
// correct version

#include "common.h"

#include "mapper.h"
#include "rom.h"

// 64 KB block, selected by 0x8000-0x9FFF. Represented as an offset in 16 KB
// units - always a multiple of four.
static unsigned block;
// 16 KB Page within block, selected by 0xA000-0xFFFF
static unsigned page;

static void make_effective() {
    set_prg_16k_bank(0, block | page);
    set_prg_16k_bank(1, block | 3);
}

void mapper_232_init() {
    // CHR fixed
    set_chr_8k_bank(0);

    block = page = 0;
    make_effective();
}

void mapper_232_write(uint8_t value, uint16_t addr) {
    if (!(addr & 0x8000)) return;

    if (!((addr >> 13) & 3))
        // 0x8000-0x9FFF
        block = (value & 0x18) >> 1;
    else
        // 0xA000-0xFFFF
        page = value & 3;

    make_effective();
}
