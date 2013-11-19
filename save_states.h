void init_save_states();
void deinit_save_states();

// True if the current frame was loaded from the rewind buffer
extern bool is_rewinding;

// These hold information about the requested state transfer

void save_state();
void load_state();

void record_state();
void rewind_state();
