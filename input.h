#include <SDL/SDL.h>

extern Uint8 *keys;

void init_input();

uint8_t get_button_states(unsigned n);
void reset_input_state();
