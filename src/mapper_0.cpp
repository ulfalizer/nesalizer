// NROM

#include "common.h"

#include "mapper.h"

static void apply_state() {}

void mapper_0_init() {
    set_prg_32k_bank(0);
    set_chr_8k_bank(0);
}

MAPPER_STATE_START(0)
MAPPER_STATE_END(0)
