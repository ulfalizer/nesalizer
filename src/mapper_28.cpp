// Multicart mapper used by Action 53,
// http://wiki.nesdev.com/w/index.php/INES_Mapper_028

// Two Verilog implementations appear at
// http://wiki.nesdev.com/w/index.php/User:Tepples/Multi-discrete_mapper/Verilog

#include "common.h"

#include "mapper.h"

// regs[0-3] correspond to R:$00, R:$01, R:$80, and R:$81 in the documentation
static uint8_t regs[4];
static unsigned regs_i;

static void apply_state() {
    set_chr_8k_bank(regs[0] & 3);

    uint8_t const outer_bank = (regs[3] & 0x3F) << 1;
    uint8_t const inner_bank = regs[1] & 0x0F;

    uint8_t const game_size = (regs[2] >> 4) & 3;
    uint8_t const mask = (2 << game_size) - 1;

    if (!(regs[2] & 0x08)) // (P)RG size
        // 32 KB PRG swapping
        set_prg_32k_bank(((outer_bank & ~mask) | ((inner_bank << 1) & mask))/2);
    else {
        // 16 KB PRG swapping
        if (!(regs[2] & 0x04)) { // (S)lot select
            set_prg_16k_bank(0, outer_bank);
            set_prg_16k_bank(1, (outer_bank & ~mask) | (inner_bank & mask));
        }
        else {
            set_prg_16k_bank(0, (outer_bank & ~mask) | (inner_bank & mask));
            set_prg_16k_bank(1, outer_bank + 1);
        }
    }

    switch (regs[2] & 3) {
    case 0: set_mirroring(ONE_SCREEN_LOW);  break;
    case 1: set_mirroring(ONE_SCREEN_HIGH); break;
    case 2: set_mirroring(VERTICAL);        break;
    case 3: set_mirroring(HORIZONTAL);      break;
    }
}

void mapper_28_init() {
    regs[0] = regs[1] = regs[2] = 0;
    regs[3] = 0x3F; // Last bank switched in
    regs_i = 0;

    apply_state();
}

void mapper_28_write(uint8_t val, uint16_t addr) {
    switch (addr) {
    case 0x5000 ... 0x5FFF:
        regs_i = ((val >> 6) & 2) | (val & 1);
        break;

    case 0x8000 ... 0xFFFF:
        regs[regs_i] = val;
        // The mirroring bit in R:$01 and R:$02 overrides the mirroring bits in
        // R:$02 if the high mirroring bit in R:$02 is 0. Implement it by
        // writing the bit directly to R:$02.
        if ((regs_i == 0 || regs_i == 1) && !(regs[2] & 2))
            regs[2] = (regs[2] & ~1) | ((val >> 4) & 1);
        break;

    default:
        return;
    }

    apply_state();
}

MAPPER_STATE_START(28)
  TRANSFER(regs)
  TRANSFER(regs_i)
MAPPER_STATE_END(28)
