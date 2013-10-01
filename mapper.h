void init_mappers();

typedef void write_fn(uint8_t value, uint16_t addr);
typedef uint8_t read_fn(uint16_t addr);
typedef void ppu_tick_callback_fn();
typedef uint8_t read_nt_fn(uint16_t addr);
typedef void write_nt_fn(uint16_t addr, uint8_t value);

struct Mapper_fns {
    void (*init)();
    read_fn *read;
    write_fn *write;
    read_nt_fn *read_nt;
    write_nt_fn *write_nt;
    ppu_tick_callback_fn *ppu_tick_callback;
};

extern uint8_t *prg_pages[4];

void set_prg_32k_bank(unsigned bank);
void set_prg_16k_bank(unsigned n, unsigned bank);
void set_prg_8k_bank(unsigned n, unsigned bank);

extern uint8_t *chr_pages[8];

void set_chr_8k_bank(unsigned bank);
void set_chr_4k_bank(unsigned n, unsigned bank);
void set_chr_2k_bank(unsigned n, unsigned bank);
void set_chr_1k_bank(unsigned n, unsigned bank);

extern uint8_t *prg_ram;

// Updating this will require updating mirroring_to_str as well
enum Mirroring {
    HORIZONTAL      = 0,
    VERTICAL        = 1,
    ONE_SCREEN_LOW  = 2,
    ONE_SCREEN_HIGH = 3,
    FOUR_SCREEN     = 4,
    SPECIAL         = 5, // Mapper-specific special mirroring

    N_MIRRORING_MODES
};
extern Mirroring mirroring;

void set_mirroring(Mirroring m);

extern Mapper_fns mapper_functions[256];

extern read_fn  *read_mapper;
extern write_fn *write_mapper;
extern ppu_tick_callback_fn *ppu_tick_callback;
extern read_nt_fn *mapper_read_nt;
extern write_nt_fn *mapper_write_nt;
