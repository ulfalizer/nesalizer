#include "common.h"

#include "cpu.h"
#include "sdl_backend.h"
#ifdef RUN_TESTS
#  include "test.h"
#endif

#include <SDL/SDL.h>
#include <SDL/SDL_version.h>

//
// Video
//

unsigned const screen_width  = 3*256;
unsigned const screen_height = 3*240;

static SDL_Surface *screen;

// Cached screen parameters
static uint32_t *pixels;
static Uint16 pixel_pitch;

// Possible optimization: Use a separate scaling step

void put_pixel(unsigned x, unsigned y, uint32_t color) {
    assert(x < 256);
    assert(y < 240);

    uint32_t *const pixel_ptr = pixels + 3*(y*pixel_pitch + x);

    // Possible optimization: Look into _mm_stream_si32
    pixel_ptr[  0              ] = color;
    pixel_ptr[  1              ] = color;
    pixel_ptr[  2              ] = color;
    pixel_ptr[  pixel_pitch    ] = color;
    pixel_ptr[  pixel_pitch + 1] = color;
    pixel_ptr[  pixel_pitch + 2] = color;
    pixel_ptr[2*pixel_pitch    ] = color;
    pixel_ptr[2*pixel_pitch + 1] = color;
    pixel_ptr[2*pixel_pitch + 2] = color;
}

void show_frame() {
    SDL_Flip(screen);
}

//
// Audio
//

Uint16 const sdl_audio_buffer_size = 2048;

// Audio ring buffer
// Make room for 1/6'th seconds of delay and round up to the nearest power of
// two for efficient wrapping
static int16_t audio_buffer[GE_POW_2(sample_rate/6)];
static size_t  start_index, end_index;

// Returns the fill level of the audio buffer in percent
double audio_buf_fill_level() {
    double const data_len = (end_index + ARRAY_LEN(audio_buffer) - start_index) % ARRAY_LEN(audio_buffer);
    return data_len/ARRAY_LEN(audio_buffer);
}

// Un-static to prevent warning
void print_fill_level() {
    static unsigned count = 0;
    if (++count == 8) {
        printf("fill level: %f%%\n",
               100.0*((end_index + ARRAY_LEN(audio_buffer) - start_index) % ARRAY_LEN(audio_buffer))/
               ARRAY_LEN(audio_buffer));
        count = 0;
    }
}

void add_audio_samples(int16_t *samples, size_t len) {
    // TODO: Copy larger chunks like in the audio callback

    SDL_LockAudio();
    for (size_t i = 0; i < (size_t)len; ++i) {
        size_t new_end_index = (end_index + 1) % ARRAY_LEN(audio_buffer);
        if (new_end_index == start_index) goto overflow;
        audio_buffer[end_index] = samples[i];
        end_index = new_end_index;
    }
    SDL_UnlockAudio();
    return;

overflow:
    SDL_UnlockAudio();
#ifndef RUN_TESTS
    puts("overflow!");
#endif
    return;
}

void start_audio_playback() {
    SDL_PauseAudio(0);
}

static void sdl_audio_callback(void*, Uint8 *stream, int len) {
    assert(len >= 0);
    assert(start_index < ARRAY_LEN(audio_buffer));

    int16_t *out = (int16_t*)stream;
    len /= sizeof(int16_t);

    //print_fill_level();

    // Copy data from the internal ring buffer to 'stream'. To speed things up,
    // copy contiguous sections with memcpy().

    unsigned const contiguous_avail
      = ((start_index <= end_index) ? end_index : ARRAY_LEN(audio_buffer)) - start_index;
    if (contiguous_avail >= (size_t)len) {
        memcpy(out, audio_buffer + start_index, sizeof(int16_t)*len);
        start_index = (start_index + len) % ARRAY_LEN(audio_buffer);
    }
    else {
        memcpy(out, audio_buffer + start_index, sizeof(int16_t)*contiguous_avail);
        len -= contiguous_avail;
        assert(len > 0);
        start_index = (start_index + contiguous_avail) % ARRAY_LEN(audio_buffer);
        assert(start_index <= end_index);
        size_t const avail = end_index - start_index;
        if (avail >= (size_t)len) {
            memcpy(out + contiguous_avail, audio_buffer + start_index, sizeof(int16_t)*len);
            start_index += len;
        }
        else {
            memcpy(out + contiguous_avail, audio_buffer + start_index, sizeof(int16_t)*avail);
            assert(start_index + avail == end_index);
            start_index = end_index;
            goto underflow;
        }
    }

    return;

underflow:
#ifndef RUN_TESTS
    puts("underflow!");
#endif
    return;
}

//
// Input
//

Uint8 *keys;

void sync_input() {
    SDL_PumpEvents();
}

//
// Events
//

void process_backend_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            end_emulation = true;
#ifdef RUN_TESTS
            end_testing = true;
#endif
        }
    }
}

//
// Initialization and de-initialization
//

void init_sdl() {
    SDL_version sdl_compiled_version;
    SDL_version const*sdl_linked_version;
    SDL_VERSION(&sdl_compiled_version);
    sdl_linked_version = SDL_Linked_Version();
    printf("Using SDL backend. Compiled against SDL %d.%d.%d, linked to SDL %d.%d.%d.\n",
           sdl_compiled_version.major, sdl_compiled_version.minor, sdl_compiled_version.patch,
           sdl_linked_version->major, sdl_linked_version->minor, sdl_linked_version->patch);

    // SDL and video

    fail_if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0,
      "unable to initialize SDL: %s", SDL_GetError());
    fail_if(!(screen = SDL_SetVideoMode(screen_width, screen_height, 32, SDL_SWSURFACE)),
      "unable to set video mode: %s", SDL_GetError());

    pixels      = (uint32_t*)screen->pixels;
    pixel_pitch = screen->pitch/4;

    // Audio

    // Avoid warnings about uninitialized (out) members
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    SDL_AudioSpec as = { sample_rate, AUDIO_S16SYS, 1 };
    #pragma GCC diagnostic pop
    as.callback = sdl_audio_callback;
    as.samples  = sdl_audio_buffer_size;

    fail_if(SDL_OpenAudio(&as, 0) < 0,
      "SDL audio initialization failed: %s", SDL_GetError());

    // Input

    // We use SDL_GetKey/MouseState() instead
    SDL_EventState(SDL_KEYDOWN        , SDL_IGNORE);
    SDL_EventState(SDL_KEYUP          , SDL_IGNORE);
    SDL_EventState(SDL_MOUSEBUTTONDOWN, SDL_IGNORE);
    SDL_EventState(SDL_MOUSEBUTTONUP  , SDL_IGNORE);
    SDL_EventState(SDL_KEYUP          , SDL_IGNORE);
    SDL_EventState(SDL_MOUSEMOTION    , SDL_IGNORE);

    keys = SDL_GetKeyState(0);
}

void deinit_sdl() {
    SDL_CloseAudio(); // Prolly not needed, but play it safe
    SDL_Quit();
}
