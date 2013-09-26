extern uint8_t *prg_base;
extern unsigned prg_16k_banks;

#define PRG(addr) prg_pages[(addr >> 13) & 3][addr & 0x1FFF]

extern bool uses_chr_ram;
extern uint8_t *chr_base;
extern unsigned chr_8k_banks;

void load_rom(char const*filename, bool print_info);
void unload_rom();
