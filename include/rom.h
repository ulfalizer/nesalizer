// Loading and unloading of ROM files

// Points to the start of the PRG data within the ROM image
extern uint8_t *prg_base;
extern unsigned prg_16k_banks;

// Points to the start of the CHR data within the ROM image, or to a
// dynamically allocated buffer if the cart uses RAM for CHR
extern uint8_t *chr_base;
extern unsigned chr_8k_banks;
extern bool chr_is_ram;

// Points to a dynamically allocated buffer for SRAM/WRAM. We usually have to
// assume the cart has SRAM/WRAM due to iNES ickiness.
extern uint8_t *wram_base;
extern unsigned wram_8k_banks;

// True if this is a PAL ROM
extern bool is_pal;

// If true, the mapper has bus conflicts and does not shut off ROM output for
// writes to the $8000+ range. This results in an AND between the written value
// and the value in ROM. Cybernoid depends on this being emulated.
extern bool has_bus_conflicts;

extern Mapper_fns mapper_fns;

// Loads a ROM file. If 'print_info' is true, information about the cart is
// printed to stdout.
void load_rom(char const *filename, bool print_info);

// Frees resources associated with the ROM
void unload_rom();
