#include <SDL.h>

// Initialization and de-initialization

void init_sdl();
void deinit_sdl();

// Main loop and signalling of SDL thread

void sdl_thread();
// Called from the emulation thread to cause the SDL thread to exit.
void exit_sdl_thread();

// Video

void put_pixel(unsigned x, unsigned y, uint32_t color);
void draw_frame();

// Audio

int const sample_rate = 44100;

void lock_audio();
void unlock_audio();

void start_audio_playback();
void stop_audio_playback();

// Input and events

void handle_ui_keys();

extern SDL_mutex *event_lock;
extern Uint8 const *keys;
