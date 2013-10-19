#include "common.h"

#include "apu.h"
#include "controller.h"
#include "cpu.h"
#include "ppu.h"
#include "mapper.h"
#include "save_states.h"

// Number of seconds of rewind to support. The rewind buffer is a ring buffer
// where a new state will overwrite the oldest state when the buffer is full.
unsigned const n_rewind_seconds = 30;

bool             pending_state_transfer;
Save_load_status save_load_status;
Rewind_status    rewind_status;

static bool     has_save;
static size_t   state_size;
static uint8_t *state;

unsigned const  n_rewind_frames = 60*n_rewind_seconds;
static uint8_t *rewind_buf;
static unsigned rewind_buf_i;
static unsigned n_recorded_frames;
bool            is_rewinding;

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

static void save_state() {
    transfer_system_state<false, true>(state);
    has_save = true;
}

static void load_state() {
    if (has_save) {
        // Clear rewind
        n_recorded_frames = 0;

        transfer_system_state<false, false>(state);
    }
}

// Saves the current state to the rewind buffer
static void record_state() {
    is_rewinding = false;

    if (n_recorded_frames < n_rewind_frames)
        ++n_recorded_frames;
    rewind_buf_i = (rewind_buf_i + 1) % n_rewind_frames;
    transfer_system_state<false, true>(rewind_buf + state_size*rewind_buf_i);
}

// Loads and removes a state from the rewind buffer
static void rewind_state() {
    is_rewinding = true;

    // If we reach the beginning of the rewind buffer, keep loading the first
    // state over and over
    if (n_recorded_frames > 1) {
        rewind_buf_i = (rewind_buf_i == 0) ? n_rewind_frames - 1 : rewind_buf_i - 1;
        --n_recorded_frames;
    }
    transfer_system_state<false, false>(rewind_buf + state_size*rewind_buf_i);
}

void do_state_transfer() {
    if (save_load_status != NO_PENDING_SAVE_OR_LOAD) {
        if (save_load_status == PENDING_SAVE)
            save_state();
        else
            load_state();
        save_load_status = NO_PENDING_SAVE_OR_LOAD;
    }

    if (rewind_status != NO_PENDING_REWIND) {
        // Make sure we always have a recorded state before calling
        // rewind_state()
        if (rewind_status == PENDING_RECORD || n_recorded_frames == 0)
            record_state();
        else
            rewind_state();
        rewind_status = NO_PENDING_REWIND;
    }
}

void init_save_states() {
    pending_state_transfer = false;
    save_load_status       = NO_PENDING_SAVE_OR_LOAD;
    rewind_status          = NO_PENDING_REWIND;

    state_size = transfer_system_state<true, false>(0);
    size_t const rewind_buf_size = state_size*n_rewind_frames;
#ifndef RUN_TESTS
    printf("Save state size: %zi bytes\nRewind buffer size: %zi bytes\n",
           state_size, rewind_buf_size);
#endif
    fail_if(!(state = new (std::nothrow) uint8_t[state_size]),
      "failed to allocate %zi-byte buffer for save state", state_size);
    fail_if(!(rewind_buf = new (std::nothrow) uint8_t[rewind_buf_size]),
      "failed to allocate %zi-byte rewind buffer", rewind_buf_size);
    rewind_buf_i = 0;
}

void deinit_save_states() {
    free_array_set_null(state);
    free_array_set_null(rewind_buf);
    n_recorded_frames = 0;
    has_save = false;
}
