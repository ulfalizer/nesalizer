#include "common.h"

#include "audio.h"
#include "apu.h"
#include "controller.h"
#include "cpu.h"
#include "input.h"
#include "mapper.h"
#ifdef RECORD_MOVIE
#  include "movie.h"
#endif
#include "ppu.h"
#include "rom.h"
#include "sdl_backend.h"
#ifdef RUN_TESTS
#  include "test.h"
#endif

#include <SDL.h>

       char const *program_name;
static char const *rom_filename;

static int emulation_thread(void*) {
    // One-time initialization of various components
    init_apu();
    init_audio();
    init_cpu();
    init_debug();
    init_input();
    init_mappers();
#ifdef RECORD_MOVIE
    init_movie();
#endif

#ifdef RUN_TESTS
    run_tests();
#else
    load_rom(rom_filename, true);
    run();
    unload_rom();
#endif
    deinit_audio();
#ifdef RECORD_MOVIE
    end_movie();
#endif

    return 0;
}

int main(int argc, char *argv[]) {
    program_name = argv[0] ? argv[0] : "nesalizer";
#ifndef RUN_TESTS
    if (argc != 2) {
        fprintf(stderr, "usage: %s <rom file>\n", program_name);
        exit(EXIT_FAILURE);
    }
#endif
    rom_filename = argv[1];

    init_sdl();

    SDL_Thread *emu_thread;
    fail_if(!(emu_thread = SDL_CreateThread(emulation_thread, "emulation", 0)),
      "failed to create emulation thread: %s", SDL_GetError());
    sdl_thread_loop();
    SDL_WaitThread(emu_thread, 0);

    deinit_sdl();
    puts("Shut down cleanly");
}
