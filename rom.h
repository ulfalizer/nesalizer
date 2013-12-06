extern uint8_t *prg_base;
extern unsigned prg_16k_banks;

extern uint8_t *prg_ram_base;
extern unsigned prg_ram_8k_banks;

extern bool     uses_chr_ram;
extern uint8_t *chr_base;
extern unsigned chr_8k_banks;

extern bool     is_pal;

extern bool     has_bus_conflicts;

void load_rom(char const*filename, bool print_info);
void unload_rom();
