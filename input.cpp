#include "common.h"

#include "sdl_backend.h"

// The input routines are still tied to SDL
#include <SDL/SDL.h>

// If true, prevent the game from seeing left+right or up+down pressed
// simultaneously, which glitches out some games. When both keys are pressed at
// the same time, pretend only the key most recently pressed is pressed.
bool const prevent_simul_left_right_or_up_down = true;

static struct Controller_data {
    // Controller state
    bool left_pushed_most_recently, up_pushed_most_recently;
    bool left_was_pushed, right_was_pushed, up_was_pushed, down_was_pushed;
    bool left_pushed, right_pushed, up_pushed, down_pushed;

    // Key bindings
    unsigned key_a, key_b, key_select, key_start, key_up, key_down, key_left, key_right;
} controller_data[2];

void init_input() {
    // Currently hardcoded

    controller_data[0].key_a      = SDLK_x;
    controller_data[0].key_b      = SDLK_z;
    controller_data[0].key_select = SDLK_RSHIFT;
    controller_data[0].key_start  = SDLK_RETURN;
    controller_data[0].key_up     = SDLK_UP;
    controller_data[0].key_down   = SDLK_DOWN;
    controller_data[0].key_left   = SDLK_LEFT;
    controller_data[0].key_right  = SDLK_RIGHT;

    controller_data[1].key_a      = SDLK_w;
    controller_data[1].key_b      = SDLK_q;
    controller_data[1].key_select = SDLK_2;
    controller_data[1].key_start  = SDLK_1;
    controller_data[1].key_up     = SDLK_i;
    controller_data[1].key_down   = SDLK_k;
    controller_data[1].key_left   = SDLK_j;
    controller_data[1].key_right  = SDLK_l;
}

// To minimize input lag, we process input events from the backend right before
// the controller state is latched for the first time in the frame. As it's a
// potentially expensive operation, we never do it more than once per frame.
static bool input_synced_this_frame;

uint8_t get_button_states(unsigned n) {
    if (!input_synced_this_frame) {
        input_synced_this_frame = true;
        sync_input();

        // Calculate the logical input state after left+right/up+down
        // elimination (if enabled)

        for (unsigned i = 0; i < 2; ++i) {
            Controller_data &c = controller_data[i];

            if (!c.left_was_pushed  && keys[c.key_left ]) c.left_pushed_most_recently = true;
            if (!c.right_was_pushed && keys[c.key_right]) c.left_pushed_most_recently = false;
            if (!c.up_was_pushed    && keys[c.key_up   ]) c.up_pushed_most_recently   = true;
            if (!c.down_was_pushed  && keys[c.key_down ]) c.up_pushed_most_recently   = false;

            if (prevent_simul_left_right_or_up_down) {
                if (keys[c.key_left] && keys[c.key_right]) {
                    c.left_pushed  =  c.left_pushed_most_recently;
                    c.right_pushed = !c.left_pushed_most_recently;
                }
                else {
                    c.left_pushed  = keys[c.key_left];
                    c.right_pushed = keys[c.key_right];
                }

                if (keys[c.key_up] && keys[c.key_down]) {
                    c.up_pushed   =  c.up_pushed_most_recently;
                    c.down_pushed = !c.up_pushed_most_recently;
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
    }

    Controller_data &cd = controller_data[n];
    return (cd.right_pushed << 7) | (cd.left_pushed     << 6) | (cd.down_pushed      << 5) |
           (cd.up_pushed    << 4) | (keys[cd.key_start] << 3) | (keys[cd.key_select] << 2) |
           (keys[cd.key_b]  << 1) |  keys[cd.key_a];
}

void reset_input_state() {
    input_synced_this_frame = false;
}
