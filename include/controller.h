// Emulation of standard NES controller

// Returns the result for a read from $4016 (n = 0, first controller port) or
// $4017 (n = 1, second controller port)
uint8_t read_controller(unsigned n);
// Sets/unsets the controller strobe, which is shared between the controller
// ports
void write_controller_strobe(bool strobe);

template<bool calculating_size, bool is_save>
void transfer_controller_state(uint8_t *&buf);
