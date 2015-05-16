#include "common.h"

#include "apu.h"
#include "audio.h"
#include "mapper.h"
#ifdef RECORD_MOVIE
#  include "movie.h"
#endif
#include "md5.h"
#include "ppu.h"
#include "rom.h"
#include "save_states.h"
#include "timing.h"

uint8_t *prg_base;
unsigned prg_16k_banks;

uint8_t *chr_base;
unsigned chr_8k_banks;
bool chr_is_ram;

uint8_t *wram_base;
unsigned wram_8k_banks;

bool is_pal;

bool has_battery;
bool has_trainer;

bool is_vs_unisystem;
bool is_playchoice_10;

bool has_bus_conflicts;

Mapper_fns mapper_fns;

static uint8_t *rom_buf;

char const *const mirroring_to_str[N_MIRRORING_MODES] =
  { "horizontal",
    "vertical",
    "one-screen, low",
    "one-screen, high",
    "four-screen" };

static void do_rom_specific_overrides();

void load_rom(char const *filename, bool print_info) {
    #define PRINT_INFO(...) do { if (print_info) printf(__VA_ARGS__); } while(0)

    size_t rom_buf_size;
    rom_buf = get_file_buffer(filename, rom_buf_size);

    //
    // Parse header
    //

    is_pal = strstr(filename, "(E)") || strstr(filename, "PAL");
    PRINT_INFO("guessing %s based on filename\n", is_pal ? "PAL" : "NTSC");

    fail_if(rom_buf_size < 16,
            "'%s' is too short to be a valid iNES file (is %zu bytes - not even enough to hold the 16-byte "
            "header)", filename, rom_buf_size);

    fail_if(!MEM_EQ(rom_buf, "NES\x1A"),
            "'%s' does not start with the expected byte sequence 'N', 'E', 'S', 0x1A", filename);

    prg_16k_banks = rom_buf[4];
    chr_8k_banks  = rom_buf[5];
    PRINT_INFO("PRG ROM size: %u KB\nCHR ROM size: %u KB\n", 16*prg_16k_banks, 8*chr_8k_banks);

    fail_if(prg_16k_banks == 0, // TODO: This makes sense for NES 2.0
            "the iNES header specifies zero banks of PRG ROM (program storage), which makes no sense");
    fail_if(!is_pow_2_or_0(prg_16k_banks) || !is_pow_2_or_0(chr_8k_banks),
            "non-power-of-two PRG and CHR sizes are not supported yet");

    size_t const min_size = 16 + 512*has_trainer + 0x4000*prg_16k_banks + 0x2000*chr_8k_banks;
    fail_if(rom_buf_size < min_size,
            "'%s' is too short to hold the specified amount of PRG (program data) and CHR (graphics data) "
            "ROM - is %zu bytes, expected at least %zu bytes (16 (header) + %s%u*16384 (PRG) + %u*8192 (CHR))",
            filename, rom_buf_size, min_size, has_trainer ? "512 (trainer) + " : "", prg_16k_banks, chr_8k_banks);

    unsigned mapper;

    // Possibly updated with the high nibble below
    mapper = rom_buf[6] >> 4;

    bool const is_nes_2_0 = (rom_buf[7] & 0x0C) == 0x08;
    PRINT_INFO(is_nes_2_0 ? "in NES 2.0 format\n" : "in iNES format\n");
    // Assume we're dealing with a corrupted header (e.g. one containing
    // "DiskDude!" in bytes 7-15) if the ROM is not in NES 2.0 format and bytes
    // 12-15 are not all zero
    if (!is_nes_2_0 && !MEM_EQ(rom_buf + 12, "\0\0\0\0"))
        PRINT_INFO("header looks corrupted (bytes 12-15 not all zero) - ignoring byte 7\n");
    else {
        is_vs_unisystem  = rom_buf[7] & 1;
        is_playchoice_10 = rom_buf[7] & 2;
        mapper |= (rom_buf[7] & 0xF0);
    }

    PRINT_INFO("mapper: %u\n", mapper);

    if (rom_buf[6] & 8)
        // The cart contains 2 KB of additional CIRAM (nametable memory) and uses
        // four-screen (linear) addressing
        mirroring = FOUR_SCREEN;
    else
        mirroring = rom_buf[6] & 1 ? VERTICAL : HORIZONTAL;

    if ((has_battery = rom_buf[6] & 2)) PRINT_INFO("has battery\n");
    if ((has_trainer = rom_buf[6] & 4)) PRINT_INFO("has trainer\n");

    //
    // Set pointers, allocate memory areas, and do misc. setup
    //

    prg_base = rom_buf + 16 + 512*has_trainer;

    // Default
    has_bus_conflicts = false;

    do_rom_specific_overrides();

    // Needs to come after a possible override
    prerender_line = is_pal ? 311 : 261;

    PRINT_INFO("mirroring: %s\n", mirroring_to_str[mirroring]);

    fail_if(!(ciram = alloc_array_init<uint8_t>(mirroring == FOUR_SCREEN ? 0x1000 : 0x800, 0xFF)),
            "failed to allocate %u bytes of nametable memory", mirroring == FOUR_SCREEN ? 0x1000 : 0x800);

    if (mirroring == FOUR_SCREEN || mapper == 7)
        // Assume no WRAM when four-screen, per
        // http://wiki.nesdev.com/w/index.php/INES_Mapper_004. Also assume no
        // WRAM for AxROM (mapper 7) as having it breaks Battletoads & Double
        // Dragon. No AxROM games use WRAM.
        wram_base = wram_6000_page = NULL;
    else {
        // iNES assumes all carts have 8 KB of WRAM. For MMC5, assume the cart
        // has 64 KB.
        wram_8k_banks = (mapper == 5) ? 8 : 1;
        fail_if(!(wram_6000_page = wram_base = alloc_array_init<uint8_t>(0x2000*wram_8k_banks, 0xFF)),
                "failed to allocate %u KB of WRAM", 8*wram_8k_banks);
    }

    if ((chr_is_ram = (chr_8k_banks == 0))) {
        // Assume cart has 8 KB of CHR RAM, except for Videomation which has 16 KB
        chr_8k_banks = (mapper == 13) ? 2 : 1;
        fail_if(!(chr_base = alloc_array_init<uint8_t>(0x2000*chr_8k_banks, 0xFF)),
                "failed to allocate %u KB of CHR RAM", 8*chr_8k_banks);
    }
    else chr_base = prg_base + 16*1024*prg_16k_banks;

    #undef PRINT_INFO

    fail_if(is_nes_2_0, "NES 2.0 not yet supported");

    fail_if(!mapper_fns_table[mapper].init, "mapper %u not supported\n", mapper);

    mapper_fns = mapper_fns_table[mapper];
    mapper_fns.init();

    // Needs to come first, as it sets NTSC/PAL timing parameters used by some
    // of the other initialization functions
    init_timing_for_rom();

    init_apu_for_rom();
    init_audio_for_rom();
    init_ppu_for_rom();
    init_save_states_for_rom();
#ifdef RECORD_MOVIE
    // Needs to know whether PAL or NTSC, so can't be done in main()
    init_movie();
#endif
}

void unload_rom() {
    // Flush any pending audio samples
    end_audio_frame();

    free_array_set_null(rom_buf);
    free_array_set_null(ciram);
    if (chr_is_ram)
        free_array_set_null(chr_base);
    free_array_set_null(wram_base);

    deinit_audio_for_rom();
    deinit_save_states_for_rom();
#ifdef RECORD_MOVIE
    end_movie();
#endif
}

// ROM detection from a PRG MD5 digest. Needed to infer and correct information
// for some ROMs.

static void correct_mirroring(Mirroring m) {
    if (mirroring != m) {
        printf("Correcting mirroring from %s to %s based on ROM checksum\n",
               mirroring_to_str[mirroring], mirroring_to_str[m]);
        mirroring = m;
    }
}

static void enable_bus_conflicts() {
    puts("Enabling bus conflicts based on ROM checksum");
    has_bus_conflicts = true;
}

static void set_pal() {
    puts("Setting PAL mode based on ROM checksum");
    is_pal = true;
}

static void do_rom_specific_overrides() {
    static MD5_CTX md5_ctx;
    static unsigned char md5[16];

    MD5_Init(&md5_ctx);
    MD5_Update(&md5_ctx, (void*)prg_base, 16*1024*prg_16k_banks);
    MD5_Final(md5, &md5_ctx);

#if 0
    for (unsigned i = 0; i < 16; ++i)
        printf("%02X", md5[i]);
    putchar('\n');
#endif

    if (MEM_EQ(md5, "\xAC\x5F\x53\x53\x59\x87\x58\x45\xBC\xBD\x1B\x6F\x31\x30\x7D\xEC"))
        // Cybernoid
        enable_bus_conflicts();
    else if (MEM_EQ(md5, "\x60\xC6\x21\xF5\xB5\x09\xD4\x14\xBB\x4A\xFB\x9B\x56\x95\xC0\x73"))
        // High hopes
        set_pal();
    else if (MEM_EQ(md5, "\x44\x6F\xCD\x30\x75\x61\x00\xA9\x94\x35\x9A\xD4\xC5\xF8\x76\x67"))
        // Rad Racer 2
        correct_mirroring(FOUR_SCREEN);
}
