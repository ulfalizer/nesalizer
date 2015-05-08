void init_mappers();

extern struct Mapper_fns {
    void    (*init)();
    uint8_t (*read)(uint16_t addr);
    void    (*write)(uint8_t val, uint16_t addr);
    uint8_t (*read_nt)(uint16_t addr);
    void    (*write_nt)(uint8_t val, uint16_t addr);
    void    (*ppu_tick_callback)();

    size_t  (*state_size)(uint8_t*&);
    size_t  (*save_state)(uint8_t*&);
    size_t  (*load_state)(uint8_t*&);
} mapper_fns_table[256];

extern uint8_t *prg_pages[4];
extern bool    prg_page_is_ram[4];
extern uint8_t *prg_ram_6000_page;

uint8_t read_prg(uint16_t addr);
void write_prg(uint16_t addr, uint8_t value);

void set_prg_32k_bank(unsigned bank);
// MMC5 can map writeable PRG RAM into the $8000+ range - hence the 'is_rom'
// argument
void set_prg_16k_bank(unsigned n, int bank, bool is_rom = true);
void set_prg_8k_bank (unsigned n, int bank, bool is_rom = true);
// PRG RAM mapped at $6000-$7FFF
void set_prg_6000_bank(unsigned bank);

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

// Helper macros for declaring mapper state that needs to be included in and
// loaded from save states
//
// A declaration like
//
//   MAPPER_STATE_START(123)
//     TRANSFER(foo)
//     TRANSFER(bar)
//   MAPPER_STATE_END(123)
//
// Expands to
//
//   template<bool calculating_size, bool is_save>
//   size_t transfer_mapper_123_state(uint8_t *&buf) {
//       uint8_t *tmp = buf;
//       transfer<calculating_size, is_save>(foo, buf);
//       transfer<calculating_size, is_save>(bar, buf);
//       if (!is_save)
//           apply_state();
//       // Return state size
//       return buf - tmp;
//    }
//    // Calculating state size
//    template size_t transfer_mapper_123_state<true, false>(uint8_t*&);
//    // Saving state to buffer
//    template size_t transfer_mapper_123_state<false, true>(uint8_t*&);
//    // Loading state from buffer
//    template size_t transfer_mapper_123_state<false, false>(uint8_t*&);
//
//  Each mapper has an apply_state(), which sets up memory mappings, etc.,
//  associated with the state.

// Helper
#define MAPPER_FN_INSTANTIATIONS(n)                                     \
  template size_t transfer_mapper_##n##_state<true, false>(uint8_t*&);  \
  template size_t transfer_mapper_##n##_state<false, true>(uint8_t*&);  \
  template size_t transfer_mapper_##n##_state<false, false>(uint8_t*&);

#define MAPPER_STATE_START(n)                         \
  template<bool calculating_size, bool is_save>       \
  size_t transfer_mapper_##n##_state(uint8_t *&buf) { \
      uint8_t *tmp = buf;

#define MAPPER_STATE_END(n)   \
      if (!is_save)           \
          apply_state();      \
      return buf - tmp;       \
  }                           \
  MAPPER_FN_INSTANTIATIONS(n)
