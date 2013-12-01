#include "common.h"

#include "audio.h"
#include "mapper.h"
#include "md5.h"
#include "ppu.h"
#include "rom.h"
#include "save_states.h"

static uint8_t *rom_buf;

uint8_t        *prg_base;
unsigned        prg_16k_banks;

uint8_t        *prg_ram_base;
unsigned        prg_ram_8k_banks;

uint8_t        *chr_base;
bool            uses_chr_ram;
unsigned        chr_8k_banks;

bool            has_battery;
bool            has_trainer;

bool            is_vs_unisystem;
bool            is_playchoice_10;

// If true, the mapper has bus conflicts and does not shut off ROM output for
// writes to the $8000+ range. This results in an AND between the written value
// and the value in ROM. Significant for some games.
bool            has_bus_conflicts;

unsigned        mapper;

char const*const mirroring_to_str[N_MIRRORING_MODES] =
  { "horizontal",
    "vertical",
    "one-screen, low",
    "one-screen, high",
    "four-screen",
    "special (internal error - should never get this here)" };

// Forward declaration
static void do_rom_specific_overrides();

void load_rom(char const*filename, bool print_info) {
    size_t rom_buf_size;
    rom_buf = get_file_buffer(filename, rom_buf_size);

    #define PRINT_INFO(...) do { if (print_info) printf(__VA_ARGS__); } while(0)

    fail_if(rom_buf_size < 16,
      "'%s' is too short to be a valid iNES file "
      "(is %zu bytes - not even enough to hold the 16-byte header)",
      filename, rom_buf_size);

    fail_if(memcmp(rom_buf, "NES\x1A", 4),
      "the iNES header in '%s' does not start with the expected byte sequence 'N', 'E', 'S', 0x1A "
      "(the corresponding bytes are instead 0x%02X, 0x%02X, 0x%02X, 0x%02X)",
      filename, rom_buf[0], rom_buf[1], rom_buf[2], rom_buf[3]);

    prg_16k_banks = rom_buf[4];
    chr_8k_banks  = rom_buf[5];
    PRINT_INFO("PRG ROM size: %u KB\nCHR ROM size: %u KB\n", 16*prg_16k_banks, 8*chr_8k_banks);

    fail_if(prg_16k_banks == 0, // TODO: This makes sense for iNES 2.0
      "the iNES header specifies zero banks of PRG ROM (program storage), which makes no sense");
    fail_if(!is_pow_2_or_0(prg_16k_banks) || !is_pow_2_or_0(chr_8k_banks),
      "non-power-of-two PRG and CHR sizes are not supported yet");

    // Check if the ROM is large enough to hold all the banks

    size_t const min_size = 16 + 512*has_trainer + 0x4000*prg_16k_banks + 0x2000*chr_8k_banks;
    if (rom_buf_size < min_size) {
        char chr_msg[18]; // sizeof(" + xxx*8192 (CHR)")
        if (chr_8k_banks)
            sprintf(chr_msg, " + %u*8192 (CHR)", chr_8k_banks);
        else
            chr_msg[0] = '\0';
        fail("'%s' is too short to hold the specified number of PRG (program data) and CHR (graphics data) "
          "banks - is %zu bytes, expected at least %zu bytes (16 (header) + %s%u*16384 (PRG)%s)",
          filename, rom_buf_size, min_size,
          has_trainer ? "512 (trainer) + " : "",
          prg_16k_banks,
          chr_msg);
    }

    // Possibly updated with the high nibble below
    mapper = rom_buf[6] >> 4;

    bool const in_ines_2_0_format = (rom_buf[7] & 0x0C) == 0x08;
    PRINT_INFO(in_ines_2_0_format ? "in iNES 2.0 format\n" : "not in iNES 2.0 format\n");
    // Assume we're dealing with a corrupted header (e.g. one containing
    // "DiskDude!" in bytes 7-15) if the ROM is not in iNES 2.0 format and
    // bytes 12-15 are not all zero
    if (!in_ines_2_0_format && memcmp(rom_buf + 12, "\0\0\0\0", 4))
        PRINT_INFO("header looks corrupted (bytes 12-15 not all zero) - ignoring byte 7\n");
    else {
        is_vs_unisystem  = rom_buf[7] & 1;
        is_playchoice_10 = rom_buf[7] & 2;
        mapper |= (rom_buf[7] & 0xF0);
    }

    PRINT_INFO("mapper: %u\n", mapper);


    // If bit 3 of flag byte 6 is set, the cart contains 2 KB of additional
    // CIRAM (nametable memory) and uses four-screen (linear) addressing
    if (rom_buf[6] & 8)
        mirroring = FOUR_SCREEN;
    else
        mirroring = rom_buf[6] & 1 ? VERTICAL : HORIZONTAL;

    if ((has_battery = rom_buf[6] & 2)) PRINT_INFO("has battery\n");
    if ((has_trainer = rom_buf[6] & 4)) PRINT_INFO("has trainer\n");

    prg_base = rom_buf + 16 + 512*has_trainer;

    // Default
    has_bus_conflicts = false;

    do_rom_specific_overrides();

    PRINT_INFO("mirroring: %s\n", mirroring_to_str[mirroring]);

    if (mirroring == FOUR_SCREEN) {
        ciram = alloc_array_init<uint8_t>(0x1000, 0xFF);
        // Assume no PRG RAM when four-screen, per
        // http://wiki.nesdev.com/w/index.php/INES_Mapper_004
        prg_ram_base = prg_ram_6000_page = 0;
    }
    else {
        ciram = alloc_array_init<uint8_t>(0x800, 0xFF);

        // Original iNES assumes all carts have 8 KB of PRG RAM. For MMC5,
        // assume the cart has 64 KB.
        prg_ram_8k_banks = (mapper == 5) ? 8 : 1;

        fail_if(!(prg_ram_base = alloc_array_init<uint8_t>(0x2000*prg_ram_8k_banks, 0xFF)),
                "failed to allocate %u KB of PRG RAM", 8*prg_ram_8k_banks);
        // Default to mapping the first page to $6000-$7FFF, which will do the right thing
        // in the usual case of there only being a single page
        prg_ram_6000_page = prg_ram_base;
    }
    fail_if(!ciram,
            "failed to allocate %u bytes of nametable memory",
            mirroring == FOUR_SCREEN ? 0x1000 : 0x800);

    if ((uses_chr_ram = (chr_8k_banks == 0))) {
        // Cart uses 8 KB of CHR RAM. Not sure about the initialization value
        // here.
        chr_8k_banks = 1;
        fail_if(!(chr_base = alloc_array_init<uint8_t>(0x2000, 0xFF)),
                "failed to allocate 8 KB of CHR RAM");
    }
    else chr_base = prg_base + 16*1024*prg_16k_banks;

    #undef PRINT_INFO

    if (in_ines_2_0_format) {
        fail("iNES 2.0 not yet supported");
        // http://wiki.nesdev.com/w/index.php/INES says byte 8 is the PRG RAM
        // size, http://wiki.nesdev.com/w/index.php/NES_2.0 that it contains
        // the submapper. The two pages seem contradictory in other places too.
        //
        // Byte 9 specifies the TV system
    }

    fail_if(!mapper_functions[mapper].init, "mapper %u not supported\n", mapper);

    mapper_functions[mapper].init();
    read_mapper       = mapper_functions[mapper].read;
    write_mapper      = mapper_functions[mapper].write;
    ppu_tick_callback = mapper_functions[mapper].ppu_tick_callback;
    mapper_read_nt    = mapper_functions[mapper].read_nt;
    mapper_write_nt   = mapper_functions[mapper].write_nt;
    mapper_state_size = mapper_functions[mapper].state_size;
    mapper_save_state = mapper_functions[mapper].save_state;
    mapper_load_state = mapper_functions[mapper].load_state;

    init_save_states();
}

void unload_rom() {
    // Flush any pending audio samples
    end_audio_frame();

    free_array_set_null(rom_buf);
    free_array_set_null(ciram);
    if (uses_chr_ram)
        free_array_set_null(chr_base);
    free_array_set_null(prg_ram_base);

    deinit_save_states();
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

static void do_rom_specific_overrides() {
    static MD5_CTX md5_ctx;
    static unsigned char md5[16];

    MD5_Init(&md5_ctx);
    MD5_Update(&md5_ctx, (void*)prg_base, 16*1024*prg_16k_banks);
    MD5_Final(md5, &md5_ctx);

    if (!memcmp(md5, "\x44\x6F\xCD\x30\x75\x61\x00\xA9\x94\x35\x9A\xD4\xC5\xF8\x76\x67", 16))
        // Rad Racer 2
        correct_mirroring(FOUR_SCREEN);
    else if (!memcmp(md5, "\xAC\x5F\x53\x53\x59\x87\x58\x45\xBC\xBD\x1B\x6F\x31\x30\x7D\xEC", 16))
        // Cybernoid
        enable_bus_conflicts();
}
