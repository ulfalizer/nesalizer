#include "common.h"

#include "apu.h"
#include "controller.h"
#include "cpu.h"
#include "ppu.h"
#include "mapper.h"
#include "save_states.h"

bool has_save;

static uint8_t *state;

template<bool calculating_size, bool is_save>
static size_t transfer_system_state(uint8_t *buf) {
    uint8_t *tmp = buf;

    transfer_apu_state<calculating_size, is_save>(buf);
    transfer_cpu_state<calculating_size, is_save>(buf);
    transfer_ppu_state<calculating_size, is_save>(buf);
    transfer_controller_state<calculating_size, is_save>(buf);

    if (calculating_size)
        mapper_state_size(buf);
    else {
        if (is_save)
            mapper_save_state(buf);
        else
            mapper_load_state(buf);
    }

    // Return size of state in bytes
    return buf - tmp;
}

void save_state() {
    transfer_system_state<false, true>(state);
    has_save = true;
}

void load_state() {
    if (has_save)
        transfer_system_state<false, false>(state);
}

void init_save_states() {
    size_t const save_size = transfer_system_state<true, false>(0);
#ifndef RUN_TESTS
    printf("Save state size: %zi\n", save_size);
#endif
    fail_if(!(state = new (std::nothrow) uint8_t[save_size]),
      "failed to allocate %zi-byte buffer for save state", save_size);
}

void deinit_save_states() {
    free_array_set_null(state);
    has_save = false;
}
