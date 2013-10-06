#include <SDL.h>

void init_input();

void calc_logical_dpad_state();
uint8_t get_button_states(unsigned n);
void reset_input_state();
