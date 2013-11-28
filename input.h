#include <SDL.h>

extern bool reset_pushed;

void init_input();

void calc_controller_state();
uint8_t get_button_states(unsigned n);
void reset_input_state();

template<bool calculating_size, bool is_save>
void transfer_input_state(uint8_t *&buf);
