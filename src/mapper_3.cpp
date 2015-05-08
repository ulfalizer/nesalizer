// CNROM board and a very similar board used for Panesian games

#include "common.h"

#include "mapper.h"
#include "rom.h"

// Actual reg is only 2 bits wide, but some homebrew ROMs (e.g.
// lolicatgirls) assume more is possible
static uint8_t chr_bank;

static void apply_state() {
    set_chr_8k_bank(chr_bank);
}

void mapper_3_init() {
    // No PRG swapping
    set_prg_32k_bank(0);
    chr_bank = 0;
    apply_state();
}

void mapper_3_write(uint8_t val, uint16_t addr) {
    if (!(addr & 0x8000)) return;

    // Cybernoid depends on bus conflicts
    if (has_bus_conflicts) val &= read_prg(addr);
    chr_bank = val;
    apply_state();
}

MAPPER_STATE_START(3)
  TRANSFER(chr_bank)
MAPPER_STATE_END(3)
