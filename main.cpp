#include "common.h"

#include "audio.h"
#include "apu.h"
#include "controller.h"
#include "cpu.h"
#include "input.h"
#include "mapper.h"
#include "ppu.h"
#include "rom.h"
#include "sdl_backend.h"
#ifdef RUN_TESTS
#  include "test.h"
#endif

char const *program_name;

#ifdef RUN_TESTS
int main(int, char *argv[]) {
#else
int main(int argc, char *argv[]) {
#endif
    program_name = argv[0] ? argv[0] : "nesalizer";
#ifndef RUN_TESTS
    if (argc != 2) {
        fprintf(stderr, "usage: %s <rom file>\n", program_name);
        exit(EXIT_FAILURE);
    }
#endif

    // One-time initialization of various components
    init_apu();
    init_audio();
    init_cpu();
    init_debug();
    init_input();
    init_mappers();

#ifdef RUN_TESTS
    init_sdl();
    run_tests();
#else
    // Load the ROM before initializing SDL so we fail early without annoying
    // window flashes
    load_rom(argv[1], true);
    init_sdl();
    run();
    unload_rom();
#endif
    deinit_audio();
    deinit_sdl();
}
