// Camerica/Capcom mapper, used by the Quattro * games

// Assume http://wiki.nesdev.com/w/index.php/Talk:INES_Mapper_232 is the
// correct version

#include "common.h"

#include "mapper.h"

// 64 KB block, selected by 0x8000-0x9FFF. Represented as an offset in 16 KB
// units - always a multiple of four.
static uint8_t block;
// 16 KB Page within block, selected by 0xA000-0xFFFF
static uint8_t page;

static void apply_state() {
    set_prg_16k_bank(0, block | page);
    set_prg_16k_bank(1, block | 3);
}

void mapper_232_init() {
    // CHR fixed
    set_chr_8k_bank(0);

    block = page = 0;
    apply_state();
}

void mapper_232_write(uint8_t val, uint16_t addr) {
    if (!(addr & 0x8000)) return;

    if (!((addr >> 13) & 3))
        // 0x8000-0x9FFF
        block = (val & 0x18) >> 1;
    else
        // 0xA000-0xFFFF
        page = val & 3;

    apply_state();
}

MAPPER_STATE_START(232)
  TRANSFER(block)
  TRANSFER(page)
MAPPER_STATE_END(232)
