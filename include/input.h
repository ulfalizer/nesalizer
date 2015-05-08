void init_input();

void calc_controller_state();
uint8_t get_button_states(unsigned n);

// For rewind to work properly across resets, the reset button needs to be
// treated as just another key whose state is saved along with the rest
extern bool reset_pushed;

template<bool calculating_size, bool is_save>
void transfer_input_state(uint8_t *&buf);
