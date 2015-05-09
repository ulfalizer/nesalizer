// MMC1

#include "common.h"

#include "mapper.h"

static unsigned temp_reg;
static unsigned nth_write;
static unsigned regs[4];

static void apply_state() {
    switch (regs[0] & 3) {
    case 0: set_mirroring(ONE_SCREEN_LOW);  break;
    case 1: set_mirroring(ONE_SCREEN_HIGH); break;
    case 2: set_mirroring(VERTICAL);        break;
    case 3: set_mirroring(HORIZONTAL);      break;
    }

    if (regs[0] & 8) {
        // 16K PRG mode
        if (regs[0] & 4) {
            // $8000 swappable, $C000 fixed to page $0F
            set_prg_16k_bank(0, regs[3] & 0x0F);
            set_prg_16k_bank(1, 0x0F);
        }
        else {
            // $8000 fixed to page $00, $C000 swappable
            set_prg_16k_bank(0, 0);
            set_prg_16k_bank(1, regs[3] & 0x0F);
        }
    }
    else
        // 32K PRG mode
        set_prg_32k_bank((regs[3] & 0x0F) >> 1);

    if (regs[0] & 0x10) {
        // 4K CHR mode
        set_chr_4k_bank(0, regs[1]);
        set_chr_4k_bank(1, regs[2]);
    }
    else
        set_chr_8k_bank(regs[1] >> 1);
}

void mapper_1_init() {
    // Specified
    regs[0] = 0x0C; // 16K PRG swapping (0x08), swapping 8000-BFFF (0x04)
    // Guess
    nth_write = temp_reg = regs[1] = regs[2] = regs[3] = 0;
    apply_state();
}

void mapper_1_write(uint8_t val, uint16_t addr) {
    // static uint64_t last_write_cycle;
    if (!(addr & 0x8000)) return;

    // Writes after the first write are ignored for writes on consecutive CPU
    // cycles. Bill & Ted's Excellent Adventure needs this.
    // TODO: This breaks the Polynes demo. Investigate if it runs on the real
    // thing.
    //if (ppu_cycle == last_write_cycle + 3) return;
    //last_write_cycle = ppu_cycle;

    if (val & 0x80) {
        nth_write = 0;
        temp_reg = 0;
        regs[0] |= 0x0C; // 16K PRG swapping (0x08), swapping 8000-BFFF (0x04)
        apply_state();
    }
    else {
        temp_reg = ((val & 1) << 4) | (temp_reg >> 1);
        if (++nth_write == 5) {
            regs[(addr >> 13) & 3] = temp_reg;
            nth_write = 0;
            temp_reg = 0;
            apply_state();
        }
    }
}

MAPPER_STATE_START(1)
  TRANSFER(temp_reg)
  TRANSFER(nth_write)
  TRANSFER(regs)
MAPPER_STATE_END(1)
