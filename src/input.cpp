#include "common.h"

#include "sdl_backend.h"

// The input routines are still tied to SDL
#include <SDL.h>

// If true, prevent the game from seeing left+right or up+down pressed
// simultaneously, which glitches out some games. When both keys are pressed at
// the same time, pretend only the key most recently pressed is pressed.
bool const prevent_simul_left_right_or_up_down = true;

static struct Controller_data {
    // Button states
    bool left_pushed, right_pushed, up_pushed, down_pushed,
         a_pushed, b_pushed, start_pushed, select_pushed;

    // Used for left+right/up+down elimination
    bool left_pushed_most_recently, up_pushed_most_recently;
    bool left_was_pushed, right_was_pushed, up_was_pushed, down_was_pushed;

    // Key bindings
    unsigned key_a, key_b, key_select, key_start, key_up, key_down, key_left, key_right;
} controller_data[2];

bool reset_pushed;

void init_input() {
    // Currently hardcoded

    controller_data[0].key_a      = SDL_SCANCODE_X;
    controller_data[0].key_b      = SDL_SCANCODE_Z;
    controller_data[0].key_select = SDL_SCANCODE_RSHIFT;
    controller_data[0].key_start  = SDL_SCANCODE_RETURN;
    controller_data[0].key_up     = SDL_SCANCODE_UP;
    controller_data[0].key_down   = SDL_SCANCODE_DOWN;
    controller_data[0].key_left   = SDL_SCANCODE_LEFT;
    controller_data[0].key_right  = SDL_SCANCODE_RIGHT;

    controller_data[1].key_a      = SDL_SCANCODE_W;
    controller_data[1].key_b      = SDL_SCANCODE_Q;
    controller_data[1].key_select = SDL_SCANCODE_2;
    controller_data[1].key_start  = SDL_SCANCODE_1;
    controller_data[1].key_up     = SDL_SCANCODE_I;
    controller_data[1].key_down   = SDL_SCANCODE_K;
    controller_data[1].key_left   = SDL_SCANCODE_J;
    controller_data[1].key_right  = SDL_SCANCODE_L;
}

void calc_controller_state() {
    SDL_LockMutex(event_lock);

    for (unsigned i = 0; i < 2; ++i) {
        Controller_data &c = controller_data[i];

        // Buttons

        c.a_pushed      = keys[c.key_a];
        c.b_pushed      = keys[c.key_b];
        c.start_pushed  = keys[c.key_start];
        c.select_pushed = keys[c.key_select];

        // D-Pad (with left+right/up+down elimination if enabled)

        if (!c.left_was_pushed  && keys[c.key_left ])
            c.left_pushed_most_recently = true;
        if (!c.right_was_pushed && keys[c.key_right])
            c.left_pushed_most_recently = false;
        if (!c.up_was_pushed    && keys[c.key_up   ])
            c.up_pushed_most_recently   = true;
        if (!c.down_was_pushed  && keys[c.key_down ])
            c.up_pushed_most_recently   = false;

        if (prevent_simul_left_right_or_up_down) {
            if (keys[c.key_left] && keys[c.key_right]) {
                c.left_pushed  =  c.left_pushed_most_recently;
                c.right_pushed = !c.left_pushed;
            }
            else {
                c.left_pushed  = keys[c.key_left];
                c.right_pushed = keys[c.key_right];
            }

            if (keys[c.key_up] && keys[c.key_down]) {
                c.up_pushed   =  c.up_pushed_most_recently;
                c.down_pushed = !c.up_pushed;
            }
            else {
                c.up_pushed   = keys[c.key_up];
                c.down_pushed = keys[c.key_down];
            }
        }
        else { // !prevent_simul_left_right_or_up_down
            c.left_pushed  = keys[c.key_left];
            c.right_pushed = keys[c.key_right];
            c.up_pushed    = keys[c.key_up];
            c.down_pushed  = keys[c.key_down];
        }

        c.left_was_pushed  = keys[c.key_left];
        c.right_was_pushed = keys[c.key_right];
        c.up_was_pushed    = keys[c.key_up];
        c.down_was_pushed  = keys[c.key_down];
    }

    reset_pushed = keys[SDL_SCANCODE_F5];

    SDL_UnlockMutex(event_lock);
}

uint8_t get_button_states(unsigned n) {
    Controller_data &c = controller_data[n];
    return (c.right_pushed << 7) | (c.left_pushed  << 6) | (c.down_pushed   << 5) |
           (c.up_pushed    << 4) | (c.start_pushed << 3) | (c.select_pushed << 2) |
           (c.b_pushed     << 1) |  c.a_pushed;
}

template<bool calculating_size, bool is_save>
void transfer_input_state(uint8_t *&buf) {
    for (unsigned i = 0; i < 2; ++i) {
        Controller_data &cd = controller_data[i];

        TRANSFER(cd.a_pushed)
        TRANSFER(cd.b_pushed)
        TRANSFER(cd.start_pushed)
        TRANSFER(cd.select_pushed)

        TRANSFER(cd.right_pushed)
        TRANSFER(cd.left_pushed)
        TRANSFER(cd.down_pushed)
        TRANSFER(cd.up_pushed)
    }

    TRANSFER(reset_pushed)
}

// Explicit instantiations

// Calculating size
template void transfer_input_state<true, false>(uint8_t*&);
// Saving state to buffer
template void transfer_input_state<false, true>(uint8_t*&);
// Loading state from buffer
template void transfer_input_state<false, false>(uint8_t*&);
