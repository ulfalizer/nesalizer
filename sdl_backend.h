#include <SDL.h>

// Initialization and de-initialization

void init_sdl();
void deinit_sdl();

// Main loop and signalling of SDL thread

void sdl_thread_loop();
// Used only when running test ROMs
void exit_sdl_thread();

// Video

void put_pixel(unsigned x, unsigned y, uint32_t color);
void draw_frame();

// Audio

double audio_buf_fill_level();
void   add_audio_samples(int16_t *samples, size_t n_samples);
void   start_audio_playback();
void   stop_audio_playback();

int    const sample_rate = 44100;

// Input and events

void handle_ui_keys();

extern SDL_mutex *event_lock;
extern Uint8 const *keys;
