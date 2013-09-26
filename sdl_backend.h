#include <SDL/SDL.h>

// Video

void put_pixel(unsigned x, unsigned y, uint32_t color);
void show_frame();

// Audio

int    const sample_rate = 44100;

double audio_buf_fill_level();
void   add_audio_samples(int16_t *samples, size_t len);
void   start_audio_playback();

// Input

extern Uint8 *keys;

void sync_input();

// Events

void process_backend_events();

// Initialization and de-initialization

void init_sdl();
void deinit_sdl();
