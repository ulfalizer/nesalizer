uint8_t read_controller(unsigned n);
void    write_controller_strobe(bool strobe);

template<bool calculating_size, bool is_save>
void transfer_controller_state(uint8_t *&buf);
