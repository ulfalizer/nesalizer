#include "common.h"

#include "cpu.h"
#include "input.h"
#include "sdl_backend.h"
#ifdef RUN_TESTS
#  include "test.h"
#endif

#include <SDL.h>

//
// Video
//

static SDL_Window *screen;
static SDL_Renderer *renderer;
static SDL_Texture *screen_tex;

// On Unity with the Nouveau driver, displaying the frame sometimes blocks for
// a very long time (to the tune of only managing 30 FPS with everything
// removed but those calls when the translucent Ubuntu menu is open, and often
// less than 60 with Firefox open too). This in turn slows down emulation and
// messes up audio. To get around it, we upload frames in the SDL thread and
// keep a back buffer for drawing into from the emulation thread while the
// frame is being uploaded (a kind of manual triple buffering). If the frame
// doesn't upload in time for the next frame, we drop it. This gives us
// automatic frame skipping in general.
Uint32 render_buf_1[240*256], render_buf_2[240*256];
Uint32 *front_buffer;
Uint32 *back_buffer;

unsigned const scale_factor = 3;

void put_pixel(unsigned x, unsigned y, uint32_t color) {
    assert(x < 256);
    assert(y < 240);

    back_buffer[256*y + x] = color;
}

static SDL_mutex *frame_lock;
static SDL_cond  *frame_available_cond;
static bool       ready_to_draw_new_frame;
static bool       frame_available;

void frame_done() {
    // Signal to the SDL thread that the frame has ended

    SDL_LockMutex(frame_lock);
    // Drop the new frame if the old one is still being rendered. This also
    // means that we drop event processing for one frame, but it's probably not
    // a huge deal.
    if (ready_to_draw_new_frame) {
        frame_available = true;
        swap(back_buffer, front_buffer);
        SDL_CondSignal(frame_available_cond);
    }
    SDL_UnlockMutex(frame_lock);
}

//
// Audio
//

static SDL_AudioDeviceID audio_device_id;

Uint16 const sdl_audio_buffer_size = 2048;

// Audio ring buffer
// Make room for 1/6'th seconds of delay and round up to the nearest power of
// two for efficient wrapping
static int16_t audio_buffer[GE_POW_2(sample_rate/6)];
static size_t  start_index, end_index;

// Returns the fill level of the audio buffer in percent
double audio_buf_fill_level() {
    double const data_len =
      (end_index + ARRAY_LEN(audio_buffer) - start_index) % ARRAY_LEN(audio_buffer);
    return data_len/ARRAY_LEN(audio_buffer);
}

// Un-static to prevent warning
void print_fill_level() {
    static unsigned count = 0;
    if (++count % 8 == 0)
        printf("Audio buffer fill level: %f%%\n", 100.0*audio_buf_fill_level());
}

void add_audio_samples(int16_t *samples, size_t len) {
    // TODO: Copy larger chunks like in the audio callback

    SDL_LockAudioDevice(audio_device_id);
    for (size_t i = 0; i < (size_t)len; ++i) {
        size_t new_end_index = (end_index + 1) % ARRAY_LEN(audio_buffer);
        if (new_end_index == start_index) {
#ifndef RUN_TESTS
            puts("overflow!");
#endif
            break;
        }
        audio_buffer[end_index] = samples[i];
        end_index = new_end_index;
    }
    SDL_UnlockAudioDevice(audio_device_id);
}

void start_audio_playback() {
    SDL_PauseAudioDevice(audio_device_id, 0);
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
            // SDL 2 requires that the entire buffer is initialized even in
            // case of underflow
            memset(out + contiguous_avail + avail, 0, len - sizeof(int16_t)*avail);
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

Uint8 const*keys;

//
// Initialization and de-initialization
//

void init_sdl() {
    SDL_version sdl_compiled_version, sdl_linked_version;
    SDL_VERSION(&sdl_compiled_version);
    SDL_GetVersion(&sdl_linked_version);
    printf("Using SDL backend. Compiled against SDL %d.%d.%d, linked to SDL %d.%d.%d.\n",
           sdl_compiled_version.major, sdl_compiled_version.minor, sdl_compiled_version.patch,
           sdl_linked_version.major, sdl_linked_version.minor, sdl_linked_version.patch);

    // SDL and video

    // Make this configurable later
    SDL_DisableScreenSaver();

    fail_if(SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO) != 0,
      "failed to initialize SDL: %s", SDL_GetError());

    fail_if(!(screen =
      SDL_CreateWindow(
        "Nesalizer",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        scale_factor*256, scale_factor*240,
        SDL_WINDOW_RESIZABLE)),
      "failed to create window: %s", SDL_GetError());

    fail_if(!(renderer = SDL_CreateRenderer(screen, -1, 0)),
      "failed to create rendering context: %s", SDL_GetError());

    // Display some information about the renderer
    SDL_RendererInfo renderer_info;
    if (SDL_GetRendererInfo(renderer, &renderer_info))
        puts("Failed to get renderer information from SDL");
    else {
        if (renderer_info.name)
            printf("renderer: uses renderer \"%s\"\n", renderer_info.name);
        if (renderer_info.flags & SDL_RENDERER_SOFTWARE)
            puts("renderer: uses software rendering");
        if (renderer_info.flags & SDL_RENDERER_ACCELERATED)
            puts("renderer: uses hardware-accelerated rendering");
        if (renderer_info.flags & SDL_RENDERER_PRESENTVSYNC)
            puts("renderer: uses vsync");
        if (renderer_info.flags & SDL_RENDERER_TARGETTEXTURE)
            puts("renderer: supports rendering to texture");
    }

    if (!SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear"))
        puts("warning: failed to set linear scaling");

    fail_if(!(screen_tex =
      SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        256, 240)),
      "failed to create texture for screen: %s", SDL_GetError());

    back_buffer  = render_buf_1;
    front_buffer = render_buf_2;

    // Audio

    SDL_AudioSpec want = {};
    want.freq     = sample_rate;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = sdl_audio_buffer_size;
    want.callback = sdl_audio_callback;

    fail_if(!(audio_device_id = SDL_OpenAudioDevice(0, 0, &want, 0, 0)),
      "failed to initialize audio: %s\n", SDL_GetError());

    // Input

    // We use SDL_GetKey/MouseState() instead
    SDL_EventState(SDL_KEYDOWN        , SDL_IGNORE);
    SDL_EventState(SDL_KEYUP          , SDL_IGNORE);
    SDL_EventState(SDL_MOUSEBUTTONDOWN, SDL_IGNORE);
    SDL_EventState(SDL_MOUSEBUTTONUP  , SDL_IGNORE);
    SDL_EventState(SDL_KEYUP          , SDL_IGNORE);
    SDL_EventState(SDL_MOUSEMOTION    , SDL_IGNORE);

    keys = SDL_GetKeyboardState(0);

    // SDL thread synchronization

    frame_lock           = SDL_CreateMutex();
    frame_available_cond = SDL_CreateCond();
}

void deinit_sdl() {
    SDL_DestroyRenderer(renderer); // Also destroys the texture
    SDL_DestroyWindow(screen);

    SDL_DestroyMutex(frame_lock);
    SDL_DestroyCond(frame_available_cond);

    SDL_CloseAudioDevice(audio_device_id); // Prolly not needed, but play it safe
    SDL_Quit();
}

// SDL thread

static bool exit_sdl_thread_loop;

static void process_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            end_emulation = true;
            exit_sdl_thread_loop = true;
#ifdef RUN_TESTS
            end_testing = true;
#endif
        }
    }
}

void sdl_thread_loop() {
    for (;;) {

        // Wait for emulation thread to signal a frame has been completed

        SDL_LockMutex(frame_lock);
        ready_to_draw_new_frame = true;
        while (!frame_available && !exit_sdl_thread_loop)
            SDL_CondWait(frame_available_cond, frame_lock);
        if (exit_sdl_thread_loop) {
            SDL_UnlockMutex(frame_lock);
            return;
        }
        frame_available = ready_to_draw_new_frame = false;
        SDL_UnlockMutex(frame_lock);

        // Process events and calculate controller input state (which might
        // need left+right/up+down elimination)

        process_events();
        calc_logical_dpad_state();

        // Draw the new frame

        fail_if(SDL_UpdateTexture(screen_tex, 0, front_buffer, 256*sizeof(Uint32)),
          "failed to update screen texture: %s", SDL_GetError());
        fail_if(SDL_RenderCopy(renderer, screen_tex, 0, 0),
          "failed to copy rendered frame to render target: %s", SDL_GetError());
        SDL_RenderPresent(renderer);
    }
}

// Used only when running test ROMs. Called from emulation thread.
void exit_sdl_thread() {
    SDL_LockMutex(frame_lock);
    exit_sdl_thread_loop = true;
    SDL_CondSignal(frame_available_cond);
    SDL_UnlockMutex(frame_lock);
}
