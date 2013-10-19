void init_save_states();
void deinit_save_states();

// True if the current frame was loaded from the rewind buffer
extern bool is_rewinding;

// True if a state transfer (save or load, ordinary or for rewind) has been
// requested. The state transfer is carried out at the next instruction
// boundary.
extern bool pending_state_transfer;

// These hold information about the requested state transfer

extern enum Save_load_status {
    PENDING_SAVE,
    PENDING_LOAD,
    NO_PENDING_SAVE_OR_LOAD
} save_load_status;

extern enum Rewind_status {
    PENDING_RECORD,
    PENDING_REWIND,
    NO_PENDING_REWIND
} rewind_status;

// Called at the next instruction boundary after a state transfer is requested
void do_state_transfer();
