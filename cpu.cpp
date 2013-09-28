#include "common.h"

#include "apu.h"
#include "controller.h"
#include "cpu.h"
#include "mapper.h"
#include "ppu.h"
#ifdef RUN_TESTS
#  include "test.h"
#endif
#include "rom.h"
#include "sdl_backend.h"
#include "timing.h"

#include <readline/history.h>
#include <readline/readline.h>

// Set true to break out of the CPU emulation loop
bool end_emulation;

#ifdef RUN_TESTS
// The system is soft-reset when this goes from 1 to 0. Used by test ROMs.
static unsigned ticks_till_reset;
// Set true to perform the reset at the next instruction boundary
static bool pending_reset;
#endif

// RAM, registers, status flags, and misc. variables {{{

static uint8_t ram[0x800];

// Possible optimization: Making some of the variables a natural size for the
// implementation architecture might be faster. CPU emulation is already
// relatively speedy though, and we wouldn't get automatic wrapping.

// Registers
static uint16_t pc;
static uint8_t a, s, x, y;

// Status flags

// The value the zero and negative flags are based on. Doing it like this turns
// the setting of these flags into a simple assignment in most cases.
//
//  - !(zero_negative_flag & 0xFF) means the zero flag is set.
//  - zero_negative_flag & 0x180 means the negative flag is set.
//
// Having zero_negative_flag & 0x100 also indicate that the negative flag is
// set allows the two flags to be set separately, which is required by the BIT
// instruction and when pulling flags from the stack.
static unsigned zero_negative_flag;

static bool carry_flag;
static bool irq_disable_flag;
static bool decimal_flag;
static bool overflow_flag;

// The byte after the opcode byte. Always fetched, so factoring out the fetch
// saves logic.
static uint8_t op_1;

// Current CPU read/write state. Needed to get the timing for APU DMC sample
// loading right (tested by the sprdma_and_dmc_dma tests).
bool cpu_is_reading;

// }}}

// PPU and APU interface {{{

void tick() {
    tick_ppu();
    tick_ppu();
    tick_ppu();

    tick_apu();

#ifdef RUN_TESTS
    if (ticks_till_reset > 0 && --ticks_till_reset == 0)
        pending_reset = true;
#endif
}

// Optimization for read/write ticks without visible side effects

static void read_tick() {
    cpu_is_reading = true;
    tick();
}

static void write_tick() {
    cpu_is_reading = false;
    tick();
}

// }}}

// Reading and writing {{{

// Forward declaration
static void poll_for_interrupt();

// Currently only used to implement open bus reads
uint8_t cpu_data_bus;

uint8_t read(uint16_t addr) {
    read_tick();

    uint8_t res;

    // Possible optimization: Checking for the most commonly read areas first
    // might be faster here

    switch (addr) {
    case 0x0000 ... 0x1FFF: res = ram[addr & 0x07FF];     break;
    case 0x2000 ... 0x3FFF: res = read_ppu_reg(addr & 7); break;

    case 0x4015: res = read_apu_status();  break;
    case 0x4016: res = read_controller(0); break;
    case 0x4017: res = read_controller(1); break;

    case 0x6000 ... 0x7FFF:
        res = prg_ram ? prg_ram[addr & 0x1FFF] : cpu_data_bus;
        break;

    case 0x8000 ... 0xFFFF: res = PRG(addr); break;

    default: res = cpu_data_bus; break;
    }

    cpu_data_bus = res;
    return res;
}

void write(uint8_t value, uint16_t addr) {
    // TODO: The write probably takes effect earlier than after the three PPU
    // ticks and the one APU tick

    write_tick();

    cpu_data_bus = value;

    switch (addr) {
    case 0x0000 ... 0x1FFF: ram[addr & 0x7FF] = value;           break;
    case 0x2000 ... 0x3FFF: write_ppu_reg(value, addr & 0x0007); break;

    // Pulse 1
    case 0x4000: write_pulse_reg_0(0, value); break;
    case 0x4001: write_pulse_reg_1(0, value); break;
    case 0x4002: write_pulse_reg_2(0, value); break;
    case 0x4003: write_pulse_reg_3(0, value); break;

    // Pulse 2
    case 0x4004: write_pulse_reg_0(1, value); break;
    case 0x4005: write_pulse_reg_1(1, value); break;
    case 0x4006: write_pulse_reg_2(1, value); break;
    case 0x4007: write_pulse_reg_3(1, value); break;

    // Triangle
    case 0x4008: write_triangle_reg_0(value); break;
    case 0x400A: write_triangle_reg_1(value); break;
    case 0x400B: write_triangle_reg_2(value); break;

    // Noise
    case 0x400C: write_noise_reg_0(value); break;
    case 0x400E: write_noise_reg_1(value); break;
    case 0x400F: write_noise_reg_2(value); break;

    // DMC
    case 0x4010: write_dmc_reg_0(value); break;
    case 0x4011: write_dmc_reg_1(value); break;
    case 0x4012: write_dmc_reg_2(value); break;
    case 0x4013: write_dmc_reg_3(value); break;

    case 0x4014: do_oam_dma(value); break;

    case 0x4015: write_apu_status(value);            break;
    case 0x4016: write_controller_strobe(value & 1); break;
    case 0x4017: write_frame_counter(value);         break;

    case 0x6000 ... 0x7FFF:
#ifdef RUN_TESTS
        // blargg's test ROMs write the test status to $6000 and a
        // corresponding text string to $6004
        if (addr == 0x6000) {
            if (value < 0x80)
                report_status_and_end_test(value, (char*)prg_ram + 4);
            else if (value == 0x81)
                // Wait 150 ms before resetting
                ticks_till_reset = 0.15*ntsc_cpu_clock_rate;
        }
#endif

        // PRG RAM/SRAM/WRAM
        if (prg_ram) prg_ram[addr & 0x1FFF] = value;
        break;

    case 0x8000 ... 0xFFFF: break;
    }

    write_mapper(value, addr);
}

// }}}

// Core instruction logic, reused for different addressing modes {{{

// Forward declarations
static void and_(uint8_t);
static uint8_t lsr(uint8_t);
static void sbc(uint8_t);

static void adc(uint8_t arg) {
    unsigned const sum = a + arg + carry_flag;
    carry_flag = sum > 0xFF;
    // The overflow flag is set when the sign of the addends is the same and
    // differs from the sign of the sum
    overflow_flag = ~(a ^ arg) & (a ^ sum) & 0x80;
    zero_negative_flag = a /* (uint8_t) */ = sum;
}

// Unofficial
static void alr(uint8_t arg) {
    a = lsr(a & arg);
}

// Unofficial
static void anc(uint8_t arg) {
    and_(arg);
    carry_flag = zero_negative_flag & 0x180;
}

// 'and' is an operator in C++, so we need the underscore
static void and_(uint8_t arg) {
    zero_negative_flag = a = a & arg;
}

// Unofficial
static void arr(uint8_t arg) {
    zero_negative_flag = a = (carry_flag << 7) | ((a & arg) >> 1);
    carry_flag = a & 0x40;
    overflow_flag = (a ^ (a << 1)) & 0x40;
}

static uint8_t asl(uint8_t arg) {
    carry_flag = arg & 0x80;
    return zero_negative_flag = (arg << 1) & 0xFF;
}

// Unofficial
static void atx(uint8_t arg) {
    // Assume '(A | 0xFF) & arg' is calculated, which is the same as just 'arg':
    // http://forums.nesdev.com/viewtopic.php?t=3831
    zero_negative_flag = x = a = arg;
}

// Unofficial
static void axs(uint8_t arg) {
    carry_flag = (a & x) >= arg;
    zero_negative_flag = x /* (uint8_t) */ = (a & x) - arg;
}

static void bit(uint8_t arg) {
    overflow_flag = arg & 0x40;
    // Set the zero and negative flags separately by using bit 8 of
    // zero_negative_flag for the negative flag
    zero_negative_flag = ((arg << 1) & 0x100) | (a & arg);
}

// CMP, CPX, CPY
static void comp(uint8_t reg, uint8_t arg) {
    carry_flag = reg >= arg;
    zero_negative_flag = (reg - arg) & 0xFF;
}

// Unofficial
static uint8_t dcp(uint8_t arg) {
    comp(a, --arg);
    return arg;
}

static uint8_t dec(uint8_t arg) {
    // Works without & 0xFF here since bit 8 (the additional negative flag bit)
    // will only get set if arg is 0, in which case bit 7 gets set as well
    return zero_negative_flag = arg - 1;
}

static void eor(uint8_t arg) {
    zero_negative_flag = (a ^= arg);
}

static uint8_t inc(uint8_t arg) {
    return zero_negative_flag = (arg + 1) & 0xFF;
}

// Unofficial
static void las(uint8_t arg) {
    zero_negative_flag = a = x = s = arg & s;
}

// Unofficial
static void lax(uint8_t arg) {
    zero_negative_flag = a = x = arg;
}

static void lda(uint8_t arg) { zero_negative_flag = a = arg; }
static void ldx(uint8_t arg) { zero_negative_flag = x = arg; }
static void ldy(uint8_t arg) { zero_negative_flag = y = arg; }

static uint8_t lsr(uint8_t arg) {
    carry_flag = arg & 1;
    return zero_negative_flag = arg >> 1;
}

static void ora(uint8_t arg) {
    zero_negative_flag = (a |= arg);
}

// Unofficial
static uint8_t isc(uint8_t arg) {
    sbc(++arg);
    return arg;
}

// Unofficial
static uint8_t rla(uint8_t arg) {
    bool const new_carry_flag = arg & 0x80;
    and_(arg = (arg << 1) | carry_flag);
    carry_flag = new_carry_flag;
    return arg;
}

static uint8_t rol(uint8_t arg) {
    bool const new_carry_flag = arg & 0x80;
    zero_negative_flag = arg /* (uint8_t) */ = (arg << 1) | carry_flag;
    carry_flag = new_carry_flag;
    return arg;
}

static uint8_t ror(uint8_t arg) {
    bool const new_carry_flag = arg & 1;
    zero_negative_flag = arg = (carry_flag << 7) | (arg >> 1);
    carry_flag = new_carry_flag;
    return arg;
}

// Unofficial
static uint8_t rra(uint8_t arg) {
    bool const new_carry_flag = arg & 1;
    arg = (carry_flag << 7) | (arg >> 1);
    carry_flag = new_carry_flag;
    adc(arg);
    return arg;
}

static void sbc(uint8_t arg) { adc(~arg); /* -arg - 1 */ }

// Unofficial
static uint8_t slo(uint8_t arg) {
    carry_flag = arg & 0x80;
    ora(arg <<= 1);
    return arg;
}

// Unofficial
static uint8_t sre(uint8_t arg) {
    carry_flag = arg & 1;
    eor(arg >>= 1);
    return arg;
}

// Unofficial
static void xaa(uint8_t arg) {
    // http://visual6502.org/wiki/index.php?title=6502_Opcode_8B_%28XAA,_ANE%29
    // Nestopia uses 0xEE as the magic constant.
    zero_negative_flag = a = (a | 0xEE) & x & arg;
}

// Conditional branches
static void branch_if(bool cond) {
    ++pc;
    if (cond) {
        read(pc); // Dummy read
        // TODO: Unsafe unsigned->signed conversion - likely to work in
        // practice
        uint16_t const new_pc = pc + (int8_t)op_1;
        if ((pc ^ new_pc) & 0x100) { // Page crossing?
            // Branch instructions perform additional interrupt polling during
            // the fixup tick
            poll_for_interrupt();
            read((pc & 0xFF00) | (new_pc & 0x00FF)); // Dummy read
        }
        pc = new_pc;
    }
}

// Stack manipulation

static void push(uint8_t value) {
    write_tick();
    ram[0x100 + s--] = value;
}

static uint8_t pull() {
    read_tick();
    return ram[0x100 + ++s];
}

static void push_flags(bool with_break_bit_set) {
    //    7        6     5   4      3            2          1     0
    // negative overflow 1 break decimal interrupt-disable zero carry
    push(
      (!!(zero_negative_flag & 0x180) << 7) |
      (overflow_flag                  << 6) |
      (1                              << 5) |
      (with_break_bit_set             << 4) |
      (decimal_flag                   << 3) |
      (irq_disable_flag               << 2) |
      (!(zero_negative_flag & 0xFF)   << 1) |
      carry_flag);
}

static void pull_flags() {
    uint8_t const flags = pull();
    // flags & 0x82  pulls out the zero and negative flags.
    //
    // ^2            flips the zero flag, since we want 0 if it's set.
    //
    // << 1          moves the negative flag into the extra negative flag bit in
    //               zero_negative_flag, so that the negative and zero flags can
    //               be set separately. The zero flag moves to bit 3, which
    //               won't affect the result.
    zero_negative_flag = ((flags & 0x82) ^ 2) << 1;
    overflow_flag      = flags & 0x40;
    decimal_flag       = flags & 0x08;
    irq_disable_flag   = flags & 0x04;
    carry_flag         = flags & 0x01;
}

// Helpers for read-modify-write instructions, which perform a dummy write-back
// of the unmodified value

#define RMW(fn, addr)                                        \
    do {                                                     \
        uint16_t const addr_ = addr;                         \
        uint8_t const val = read(addr_);                     \
        write(val, addr_); /* Write back unmodified value */ \
        poll_for_interrupt();                                \
        write(fn(val), addr_);                               \
    } while(0)

// Optimized versions for zero page access, which never has side effects

#define ZERO_RMW(fn)                                   \
    do {                                               \
        ++pc;                                          \
        read_tick(); /* Read effective address */      \
        read_tick(); /* Write back unmodified value */ \
        poll_for_interrupt();                          \
        write_tick();                                  \
        ram[op_1] = fn(ram[op_1]);                     \
    } while(0)

#define ZERO_X_RMW(fn)                                  \
    do {                                                \
        ++pc;                                           \
        uint8_t const addr = op_1 + x;                  \
        read_tick(); /* Read address and add x to it */ \
        read_tick(); /* Read effective address */       \
        read_tick(); /* Write back unmodified value */  \
        poll_for_interrupt();                           \
        write_tick();                                   \
        ram[addr] = fn(ram[addr]);                      \
    } while(0)

// }}}

// Addressing modes {{{

// The _addr functions return addresses, the _op functions resolved operands

//
// Zero page addressing
//

static uint8_t get_zero_op() {
    ++pc;
    poll_for_interrupt();
    read_tick();
    return ram[op_1];
}

static uint8_t get_zero_xy_op(uint8_t index) {
    ++pc;
    read_tick(); // Read from address, add index
    poll_for_interrupt();
    read_tick();
    return ram[(op_1 + index) & 0xFF];
}

// Writing zero page never has side effects, so we can optimize a bit

static void zero_write(uint8_t val) {
    ++pc;
    poll_for_interrupt();
    write_tick();
    ram[op_1] = val;
}

static void zero_xy_write(uint8_t val, uint8_t index) {
    ++pc;
    read_tick(); // Read from address and add x to it
    poll_for_interrupt();
    write_tick();
    ram[(op_1 + index) & 0xFF] = val;
}

//
// Absolute addressing
//

static uint16_t get_abs_addr() {
    ++pc;
    return (read(pc++) << 8) | op_1;
}

static uint8_t get_abs_op() {
    uint16_t const addr = get_abs_addr();
    poll_for_interrupt();
    return read(addr);
}

static void abs_write(uint8_t val) {
    uint16_t const addr = get_abs_addr();
    poll_for_interrupt();
    write(val, addr);
}

//
// Absolute,X/Y addressing
//

// Absolute,X/Y address fetching for write and read-modify-write instructions
static uint16_t get_abs_xy_addr_write(uint8_t index) {
    uint16_t const addr = get_abs_addr();
    read((addr & 0xFF00) | ((addr + index) & 0x00FF)); // Dummy read
    return addr + index;
}

// Absolute,X/Y operand fetching for read instructions
static uint8_t get_abs_xy_op_read(uint8_t index) {
    uint16_t const addr     = get_abs_addr();
    uint16_t const new_addr = addr + index;
    if ((addr ^ new_addr) & 0x100) // Page crossing?
        read(new_addr - 0x100); // Dummy read
    poll_for_interrupt();
    return read(new_addr);
}

// Instructions that use this always write the accumulator, so we can omit the
// 'val' argument
static void abs_xy_write_a(uint8_t index) {
    uint16_t const addr = get_abs_xy_addr_write(index);
    poll_for_interrupt();
    write(a, addr);
}

//
// (Indirect,X) addressing
//

static uint16_t get_ind_x_addr() {
    ++pc;
    read_tick(); // Read from address, add index
    read_tick(); // Fetch effective address low
    read_tick(); // Fetch effective address high
    uint8_t const zero_addr = op_1 + x;
    return (ram[(zero_addr + 1) & 0xFF] << 8) | ram[zero_addr];
}

static uint8_t get_ind_x_op() {
    uint16_t const addr = get_ind_x_addr();
    poll_for_interrupt();
    return read(addr);
}

static void ind_x_write(uint8_t val) {
    uint16_t const addr = get_ind_x_addr();
    poll_for_interrupt();
    write(val, addr);
}

//
// (Indirect),Y addressing
//

// (Indirect),Y helper for fetching the address from zero page
static uint16_t get_addr_from_zero_page() {
    ++pc;
    read_tick(); // Fetch effective address low
    read_tick(); // Fetch effective address high
    return (ram[(op_1 + 1) & 0xFF] << 8) | ram[op_1];
}

// (Indirect),Y address fetching for write and read-modify-write instructions
static uint16_t get_ind_y_addr_write() {
    uint16_t const addr = get_addr_from_zero_page();
    read((addr & 0xFF00) | ((addr + y) & 0x00FF)); // Dummy read
    return addr + y;
}

// (Indirect),Y operand fetching for read instructions
static uint8_t get_ind_y_op_read() {
    uint16_t const addr = get_addr_from_zero_page();
    uint16_t const new_addr = addr + y;
    if ((addr ^ new_addr) & 0x100) // Page crossing?
        read(new_addr - 0x100); // Dummy read
    poll_for_interrupt();
    return read(new_addr);
}

// Single caller, always writes accumulator
static void ind_y_write_a() {
    uint16_t const addr = get_ind_y_addr_write();
    poll_for_interrupt();
    write(a, addr);
}

// Helper function for implementing the weird unofficial write instructions
// (AXA, XAS, TAS, SAY) that are influenced by the high byte of the address
// plus one (due to an internal bus conflict). In page crossings, the high byte
// of the target address is corrupted similarly to the value.
static void unoff_addr_write(uint16_t addr, uint8_t value, uint8_t index) {
    uint16_t const new_addr = addr + index;
    read((addr & 0xFF00) | (new_addr & 0x00FF)); // Dummy read
    poll_for_interrupt();
    write(value & ((addr >> 8) + 1),
          ((addr ^ new_addr) & 0x100) ?
            (new_addr & (value << 8)) | (new_addr & 0x00FF) : // Page crossing
            new_addr);                                        // No page crossing
}

// }}}

// Opcodes {{{

enum {
  // Implied
  BRK = 0x00, CLC = 0x18, CLD = 0xD8, CLI = 0x58, CLV = 0xB8, DEX = 0xCA, DEY = 0x88,
  INX = 0xE8, INY = 0xC8, NO0 = 0x1A, NO1 = 0x3A, NO2 = 0x5A, NO3 = 0x7A, NO4 = 0xDA,
  NO5 = 0xFA, NOP = 0xEA, PHA = 0x48, PHP = 0x08, PLA = 0x68, PLP = 0x28, RTI = 0x40,
  RTS = 0x60, SEC = 0x38, SED = 0xF8, SEI = 0x78, TAX = 0xAA, TAY = 0xA8, TSX = 0xBA,
  TXA = 0x8A, TXS = 0x9A, TYA = 0x98,

  // Accumulator
  ASL_ACC = 0x0A, LSR_ACC = 0x4A, ROL_ACC = 0x2A, ROR_ACC = 0x6A,

  // Immediate
  ADC_IMM = 0x69, ALR_IMM = 0x4B, AN0_IMM = 0x0B, AN1_IMM = 0x2B, AND_IMM = 0x29,
  ARR_IMM = 0x6B, ATX_IMM = 0xAB, AXS_IMM = 0xCB, CMP_IMM = 0xC9, CPX_IMM = 0xE0,
  CPY_IMM = 0xC0, EOR_IMM = 0x49, LDA_IMM = 0xA9, LDX_IMM = 0xA2, LDY_IMM = 0xA0,
  NO0_IMM = 0x80, NO1_IMM = 0x82, NO2_IMM = 0x89, NO3_IMM = 0xC2, NO4_IMM = 0xE2,
  ORA_IMM = 0x09, SB2_IMM = 0xEB, SBC_IMM = 0xE9, XAA_IMM = 0x8B,

  // Absolute
  ADC_ABS = 0x6D, AND_ABS = 0x2D, ASL_ABS = 0x0E, BIT_ABS = 0x2C, CMP_ABS = 0xCD,
  CPX_ABS = 0xEC, CPY_ABS = 0xCC, DCP_ABS = 0xCF, DEC_ABS = 0xCE, EOR_ABS = 0x4D,
  INC_ABS = 0xEE, ISC_ABS = 0xEF, JMP_ABS = 0x4C, JSR_ABS = 0x20, LAX_ABS = 0xAF,
  LDA_ABS = 0xAD, LDX_ABS = 0xAE, LDY_ABS = 0xAC, LSR_ABS = 0x4E, NOP_ABS = 0x0C,
  ORA_ABS = 0x0D, RLA_ABS = 0x2F, ROL_ABS = 0x2E, ROR_ABS = 0x6E, RRA_ABS = 0x6F,
  SAX_ABS = 0x8F, SBC_ABS = 0xED, SLO_ABS = 0x0F, SRE_ABS = 0x4F, STA_ABS = 0x8D,
  STX_ABS = 0x8E, STY_ABS = 0x8C,

  // Absolute,X
  ADC_ABS_X = 0x7D, AND_ABS_X = 0x3D, ASL_ABS_X = 0x1E, CMP_ABS_X = 0xDD,
  DCP_ABS_X = 0xDF, DEC_ABS_X = 0xDE, EOR_ABS_X = 0x5D, INC_ABS_X = 0xFE,
  ISC_ABS_X = 0xFF, LDA_ABS_X = 0xBD, LDY_ABS_X = 0xBC, LSR_ABS_X = 0x5E,
  ORA_ABS_X = 0x1D, NO0_ABS_X = 0x1C, NO1_ABS_X = 0x3C, NO2_ABS_X = 0x5C,
  NO3_ABS_X = 0x7C, NO4_ABS_X = 0xDC, NO5_ABS_X = 0xFC, RLA_ABS_X = 0x3F,
  ROL_ABS_X = 0x3E, ROR_ABS_X = 0x7E, RRA_ABS_X = 0x7F, SAY_ABS_X = 0x9C,
  SBC_ABS_X = 0xFD, SLO_ABS_X = 0x1F, SRE_ABS_X = 0x5F, STA_ABS_X = 0x9D,

  // Absolute,Y
  ADC_ABS_Y = 0x79, AND_ABS_Y = 0x39, AXA_ABS_Y = 0x9F, CMP_ABS_Y = 0xD9,
  DCP_ABS_Y = 0xDB, EOR_ABS_Y = 0x59, ISC_ABS_Y = 0xFB, LAS_ABS_Y = 0xBB,
  LAX_ABS_Y = 0xBF, LDA_ABS_Y = 0xB9, LDX_ABS_Y = 0xBE, ORA_ABS_Y = 0x19,
  RLA_ABS_Y = 0x3B, RRA_ABS_Y = 0x7B, SBC_ABS_Y = 0xF9, SLO_ABS_Y = 0x1B,
  SRE_ABS_Y = 0x5B, STA_ABS_Y = 0x99, TAS_ABS_Y = 0x9B, XAS_ABS_Y = 0x9E,

  // Zero page
  ADC_ZERO = 0x65, AND_ZERO = 0x25, BIT_ZERO = 0x24, CMP_ZERO = 0xC5,
  CPX_ZERO = 0xE4, CPY_ZERO = 0xC4, DCP_ZERO = 0xC7, EOR_ZERO = 0x45,
  ISC_ZERO = 0xE7, LAX_ZERO = 0xA7, LDA_ZERO = 0xA5, LDX_ZERO = 0xA6,
  LDY_ZERO = 0xA4, NO0_ZERO = 0x04, NO1_ZERO = 0x44, NO2_ZERO = 0x64,
  ORA_ZERO = 0x05, RLA_ZERO = 0x27, RRA_ZERO = 0x67, SBC_ZERO = 0xE5,
  SLO_ZERO = 0x07, SRE_ZERO = 0x47, ASL_ZERO = 0x06, LSR_ZERO = 0x46,
  ROL_ZERO = 0x26, ROR_ZERO = 0x66, INC_ZERO = 0xE6, DEC_ZERO = 0xC6,
  SAX_ZERO = 0x87, STA_ZERO = 0x85, STX_ZERO = 0x86, STY_ZERO = 0x84,

  // Zero page,X
  ADC_ZERO_X = 0x75, AND_ZERO_X = 0x35, ASL_ZERO_X = 0x16, CMP_ZERO_X = 0xD5,
  DCP_ZERO_X = 0xD7, DEC_ZERO_X = 0xD6, EOR_ZERO_X = 0x55, INC_ZERO_X = 0xF6,
  ISC_ZERO_X = 0xF7, LDA_ZERO_X = 0xB5, LDY_ZERO_X = 0xB4, LSR_ZERO_X = 0x56,
  NO0_ZERO_X = 0x14, NO1_ZERO_X = 0x34, NO2_ZERO_X = 0x54, NO3_ZERO_X = 0x74,
  NO4_ZERO_X = 0xD4, NO5_ZERO_X = 0xF4, ORA_ZERO_X = 0x15, RLA_ZERO_X = 0x37,
  ROL_ZERO_X = 0x36, ROR_ZERO_X = 0x76, RRA_ZERO_X = 0x77, SBC_ZERO_X = 0xF5,
  SLO_ZERO_X = 0x17, SRE_ZERO_X = 0x57, STA_ZERO_X = 0x95, STY_ZERO_X = 0x94,

  // Zero page,Y
  LAX_ZERO_Y = 0xB7, LDX_ZERO_Y = 0xB6, SAX_ZERO_Y = 0x97, STX_ZERO_Y = 0x96,

  // (Indirect,X)
  ADC_IND_X = 0x61, AND_IND_X = 0x21, CMP_IND_X = 0xC1, DCP_IND_X = 0xC3,
  EOR_IND_X = 0x41, ISC_IND_X = 0xE3, LAX_IND_X = 0xA3, LDA_IND_X = 0xA1,
  ORA_IND_X = 0x01, RLA_IND_X = 0x23, RRA_IND_X = 0x63, SAX_IND_X = 0x83,
  SBC_IND_X = 0xE1, SLO_IND_X = 0x03, SRE_IND_X = 0x43, STA_IND_X = 0x81,

  // (Indirect),Y
  ADC_IND_Y = 0x71, AND_IND_Y = 0x31, AXA_IND_Y = 0x93, CMP_IND_Y = 0xD1,
  DCP_IND_Y = 0xD3, EOR_IND_Y = 0x51, ISC_IND_Y = 0xF3, LAX_IND_Y = 0xB3,
  LDA_IND_Y = 0xB1, ORA_IND_Y = 0x11, RLA_IND_Y = 0x33, RRA_IND_Y = 0x73,
  SBC_IND_Y = 0xF1, SLO_IND_Y = 0x13, SRE_IND_Y = 0x53, STA_IND_Y = 0x91,

  // Relative (branch instructions)
  BCC = 0x90, BCS = 0xB0, BEQ = 0xF0, BMI = 0x30, BNE = 0xD0, BPL = 0x10,
  BVC = 0x50, BVS = 0x70,

  // Indirect (indirect jump)
  JMP_IND = 0x6C,

  // KIL instructions
  KI0 = 0x02, KI1 = 0x12, KI2 = 0x22, KI3 = 0x32, KI4 = 0x42, KI5 = 0x52,
  KI6 = 0x62, KI7 = 0x72, KI8 = 0x92, KI9 = 0xB2, K10 = 0xD2, K11 = 0xF2
};

// }}}

// Interrupts {{{

// Interrupt variables. 'true' means asserted.

// IRQ from mapper hardware on the cart
bool cart_irq;

// IRQ from the DMC channel
// Set by the last sample byte being loaded, unless inhibited or looping is set
// Cleared by
//  * the reset signal,
//  * writing $4015,
//  * and clearing the IRQ enable flag in $4010
bool dmc_irq;

// IRQ from the frame counter
// Set by the frame counter in 4-step mode, unless inhibited
// Cleared by (derived from Visual 2A03)
//  * the reset signal,
//  * setting the inhibit IRQ flag,
//  * and reading $4015
bool frame_irq;

// The OR of the above IRQ sources. Updated in update_irq_status().
static bool irq_line;

// Set true when a falling edge occurs on the NMI input
bool nmi_asserted;

// Set true if interrupt polling detects a pending IRQ or NMI. The next
// "instruction" executed is the interrupt sequence in that case.
static bool pending_irq;
static bool pending_nmi;

enum Interrupt_type { Int_BRK = 0, Int_IRQ, Int_NMI, Int_reset };

static void do_interrupt(Interrupt_type type) {
    static uint16_t const vector_addr[] =
      { 0xFFFE,   // Int_BRK
        0xFFFE,   // Int_IRQ
        0xFFFA }; // Int_NMI

    uint16_t vec_addr;

    // Two dummy reads
    if (type != Int_BRK) {
        // For BRK, these have already been done
        read(pc);
        read(pc);
    }

    if (type == Int_reset) {
        // Push operations, writes inhibited
        read_tick();
        read_tick();
        read_tick();
        s -= 3;
        vec_addr = 0xFFFC;
    }
    else {
        push(pc >> 8);
        push(pc & 0xFF);

        // Interrupt glitch. An NMI asserted here can override BRK and IRQ.
        if (nmi_asserted) {
            nmi_asserted = false;
            vec_addr     = 0xFFFA;
        }
        else
            vec_addr = vector_addr[(unsigned)type];

        push_flags(type == Int_BRK);
    }
    irq_disable_flag = true;
    // No interrupt polling happens here; the first instruction of the
    // interrupt handler always executes before another interrupt is serviced
    pc  = read(vec_addr);
    pc |= (read(vec_addr + 1) << 8);
}

// The interrupt lines are polled at the end of the second-to-last tick for
// most instructions, making interrupts a bit less straightforward to implement
// compared to if the polling was done at the very end. For addressing modes
// where all instructions poll interrupts in the same location we do the
// polling in the addressing mode routine to save code. Same goes for
// read-modify-write instructions, which all poll in the same location.
static void poll_for_interrupt() {
    // If both NMI and IRQ have been asserted, the IRQ assertion is lost at
    // this polling point (as if IRQ had never been asserted). IRQ might still
    // be detected at the next polling point.
    //
    // This behavior has been confirmed in Visual 6502.
    if (nmi_asserted) {
        nmi_asserted = false;
        pending_nmi  = true;
    }
    else if (irq_line && !irq_disable_flag)
        pending_irq = true;
}

void update_irq_status() {
    irq_line = cart_irq || dmc_irq || frame_irq;
}

// Probably no nice way to initialize this statically. Designated initializers
// are only supported for C in GCC.
static bool polls_irq_after_first_cycle[256];

static void init_interrupts() {
    #define P(opcode) polls_irq_after_first_cycle[opcode] = true;

    // For CLI and SEI, the flag changes after the interrupt is polled, so both
    // can safely go in this list (the polling is influenced by the current
    // flag status like for the rest)

    P(ADC_IMM) P(ALR_IMM) P(AN0_IMM) P(AN1_IMM) P(AND_IMM) P(ARR_IMM) P(ASL_ACC)
    P(BCC)     P(ATX_IMM) P(AXS_IMM) P(BCS)     P(BEQ)     P(BMI)     P(BNE)
    P(BPL)     P(BVC)     P(BVS)     P(CLC)     P(CLD)     P(CLI)     P(CLV)
    P(CMP_IMM) P(CPX_IMM) P(CPY_IMM) P(DEX)     P(DEY)     P(EOR_IMM) P(INX)
    P(INY)     P(LDA_IMM) P(LDX_IMM) P(LDY_IMM) P(LSR_ACC) P(NO0)     P(NO0_IMM)
    P(NO1_IMM) P(NO2_IMM) P(NO3_IMM) P(NO4_IMM) P(NO1)     P(NO2)     P(NO3)
    P(NO4)     P(NO5)     P(NOP)     P(ORA_IMM) P(ROL_ACC) P(ROR_ACC) P(SB2_IMM)
    P(SBC_IMM) P(SEC)     P(SED)     P(SEI)     P(TAX)     P(TAY)     P(TSX)
    P(TXA)     P(TXS)     P(TYA)     P(XAA_IMM)

    #undef P
}

// }}}

// Main CPU loop {{{

#ifdef LOG_INSTRUCTIONS
static void log_instruction();
#endif
static void set_cpu_cold_boot_state();
#ifdef RUN_TESTS
// We currently only reset for test ROMs
static void reset_cpu();
#endif

void run() {
    end_emulation = false;

    set_apu_cold_boot_state();
    set_cpu_cold_boot_state();
    set_ppu_cold_boot_state();
    init_timing();

    do_interrupt(Int_reset);
    while (!end_emulation) {

#ifdef RUN_TESTS
        if (pending_reset) {
            pending_reset = false;

            // Reset the APU and PPU first since they should tick during the
            // CPU's reset sequence
            reset_apu();
            reset_ppu();
            reset_cpu();
        }
#endif

#ifdef LOG_INSTRUCTIONS
        log_instruction();
#endif

        if (pending_nmi) {
            do_interrupt(Int_NMI);
            pending_nmi = false;
            continue;
        }

        if (pending_irq) {
            do_interrupt(Int_IRQ);
            pending_irq = false;
            continue;
        }

        uint8_t const opcode = read(pc++);
        if (polls_irq_after_first_cycle[opcode])
            poll_for_interrupt();
        op_1 = read(pc);

        // http://eli.thegreenplace.net/2012/07/12/computed-goto-for-efficient-dispatch-tables/
        // could possibly speed this up a bit (also,
        // https://www.cs.tcd.ie/David.Gregg/papers/toplas05.pdf). CPU
        // emulation seems to account for less than 5% of the runtime though,
        // so it might not be worth uglifying the code for.

        switch (opcode) {

        //
        // Accumulator or implied addressing
        //

        case BRK:
            ++pc;
            do_interrupt(Int_BRK);
            break;

        case RTI:
            {
            read_tick(); // Corresponds to incrementing s
            pull_flags();
            pc = pull();
            poll_for_interrupt();
            pc |= (pull() << 8);
            }
            break;

        case RTS:
            {
            read_tick(); // Corresponds to incrementing s
            uint8_t const pc_low = pull();
            pc = ((pull() << 8) | pc_low) + 1;
            poll_for_interrupt();
            read_tick(); // Increment PC
            }
            break;

        case PHA:
            poll_for_interrupt();
            push(a);
            break;

        case PHP:
            poll_for_interrupt();
            push_flags(true);
            break;

        case PLA:
            read_tick(); // Corresponds to incrementing s
            poll_for_interrupt();
            zero_negative_flag = a = pull();
            break;

        case PLP:
            read_tick(); // Corresponds to incrementing s
            poll_for_interrupt();
            pull_flags();
            break;

        case ASL_ACC: a = asl(a); break;
        case LSR_ACC: a = lsr(a); break;
        case ROL_ACC: a = rol(a); break;
        case ROR_ACC: a = ror(a); break;

        case CLC: carry_flag       = false; break;
        case CLD: decimal_flag     = false; break;
        case CLI: irq_disable_flag = false; break;
        case CLV: overflow_flag    = false; break;
        case SEC: carry_flag       = true;  break;
        case SED: decimal_flag     = true;  break;
        case SEI: irq_disable_flag = true;  break;

        case DEX: zero_negative_flag = --x; break;
        case DEY: zero_negative_flag = --y; break;
        case INX: zero_negative_flag = ++x; break;
        case INY: zero_negative_flag = ++y; break;

        case TAX: zero_negative_flag = x = a; break;
        case TAY: zero_negative_flag = y = a; break;
        case TSX: zero_negative_flag = x = s; break;
        case TXA: zero_negative_flag = a = x; break;
        case TXS:                      s = x; break;
        case TYA: zero_negative_flag = a = y; break;

        // The "official" NOP
        case NOP:
        // Various unofficial NOPs with accumulator/implied addressing
        case NO0: case NO1: case NO2: case NO3: case NO4: case NO5: break;

        //
        // Immediate addressing
        //

        case ADC_IMM: adc(op_1);     ++pc; break;
        case ALR_IMM: alr(op_1);     ++pc; break; // Unofficial
        case AN0_IMM: anc(op_1);     ++pc; break; // Unofficial
        case AN1_IMM: anc(op_1);     ++pc; break; // Unofficial
        case AND_IMM: and_(op_1);    ++pc; break;
        case ARR_IMM: arr(op_1);     ++pc; break; // Unofficial
        case ATX_IMM: atx(op_1);     ++pc; break; // Unofficial
        case AXS_IMM: axs(op_1);     ++pc; break; // Unofficial
        case CMP_IMM: comp(a, op_1); ++pc; break;
        case CPX_IMM: comp(x, op_1); ++pc; break;
        case CPY_IMM: comp(y, op_1); ++pc; break;
        case EOR_IMM: eor(op_1);     ++pc; break;
        case LDA_IMM: lda(op_1);     ++pc; break;
        case LDX_IMM: ldx(op_1);     ++pc; break;
        case LDY_IMM: ldy(op_1);     ++pc; break;
        case ORA_IMM: ora(op_1);     ++pc; break;
        case SB2_IMM: // Unofficial, same as SBC
        case SBC_IMM: sbc(op_1);     ++pc; break;
        case XAA_IMM: xaa(op_1);     ++pc; break; // Unofficial

        // Unofficial NOPs with immediate addressing
        case NO0_IMM: case NO1_IMM: case NO2_IMM: case NO3_IMM: case NO4_IMM: ++pc; break;

        //
        // Absolute addressing
        //

        case JMP_ABS:
            poll_for_interrupt();
            pc = (read(pc + 1) << 8) | op_1;
            break;

        case JSR_ABS:
            ++pc;

            read_tick(); // Internal operation

            push(pc >> 8);
            push(pc & 0xFF);

            poll_for_interrupt();
            pc = (read(pc) << 8) | op_1;
            break;

        // Read instructions

        case ADC_ABS: adc(get_abs_op());     break;
        case AND_ABS: and_(get_abs_op());    break;
        case BIT_ABS: bit(get_abs_op());     break;
        case CMP_ABS: comp(a, get_abs_op()); break;
        case CPX_ABS: comp(x, get_abs_op()); break;
        case CPY_ABS: comp(y, get_abs_op()); break;
        case EOR_ABS: eor(get_abs_op());     break;
        case LAX_ABS: lax(get_abs_op());     break; // Unofficial
        case LDA_ABS: lda(get_abs_op());     break;
        case LDX_ABS: ldx(get_abs_op());     break;
        case LDY_ABS: ldy(get_abs_op());     break;
        case ORA_ABS: ora(get_abs_op());     break;
        case SBC_ABS: sbc(get_abs_op());     break;

        // Unofficial NOP with absolute addressing (acts like a read)
        case NOP_ABS: get_abs_op(); break;

        // Read-modify-write instructions

        case ASL_ABS: RMW(asl, get_abs_addr()); break;
        case DCP_ABS: RMW(dcp, get_abs_addr()); break; // Unofficial
        case DEC_ABS: RMW(dec, get_abs_addr()); break;
        case INC_ABS: RMW(inc, get_abs_addr()); break;
        case ISC_ABS: RMW(isc, get_abs_addr()); break; // Unofficial
        case LSR_ABS: RMW(lsr, get_abs_addr()); break;
        case RLA_ABS: RMW(rla, get_abs_addr()); break; // Unofficial
        case RRA_ABS: RMW(rra, get_abs_addr()); break; // Unofficial
        case ROL_ABS: RMW(rol, get_abs_addr()); break;
        case ROR_ABS: RMW(ror, get_abs_addr()); break;
        case SLO_ABS: RMW(slo, get_abs_addr()); break; // Unofficial
        case SRE_ABS: RMW(sre, get_abs_addr()); break; // Unofficial

        // Write instructions

        case SAX_ABS: abs_write(a & x); break; // Unofficial
        case STA_ABS: abs_write(a);     break;
        case STX_ABS: abs_write(x);     break;
        case STY_ABS: abs_write(y);     break;

        //
        // Zero page addressing
        //

        // Read instructions

        case ADC_ZERO: adc(get_zero_op());     break;
        case AND_ZERO: and_(get_zero_op());    break;
        case BIT_ZERO: bit(get_zero_op());     break;
        case CMP_ZERO: comp(a, get_zero_op()); break;
        case CPX_ZERO: comp(x, get_zero_op()); break;
        case CPY_ZERO: comp(y, get_zero_op()); break;
        case EOR_ZERO: eor(get_zero_op());     break;
        case LAX_ZERO: lax(get_zero_op());     break; // Unofficial
        case LDA_ZERO: lda(get_zero_op());     break;
        case LDX_ZERO: ldx(get_zero_op());     break;
        case LDY_ZERO: ldy(get_zero_op());     break;
        case ORA_ZERO: ora(get_zero_op());     break;
        case SBC_ZERO: sbc(get_zero_op());     break;

        // Read-modify-write instructions

        case ASL_ZERO: ZERO_RMW(asl); break;
        case DCP_ZERO: ZERO_RMW(dcp); break; // Unofficial
        case DEC_ZERO: ZERO_RMW(dec); break;
        case INC_ZERO: ZERO_RMW(inc); break;
        case ISC_ZERO: ZERO_RMW(isc); break; // Unofficial
        case LSR_ZERO: ZERO_RMW(lsr); break;
        case RLA_ZERO: ZERO_RMW(rla); break; // Unofficial
        case RRA_ZERO: ZERO_RMW(rra); break; // Unofficial
        case ROL_ZERO: ZERO_RMW(rol); break;
        case ROR_ZERO: ZERO_RMW(ror); break;
        case SLO_ZERO: ZERO_RMW(slo); break; // Unofficial
        case SRE_ZERO: ZERO_RMW(sre); break; // Unofficial

        // Write instructions

        case SAX_ZERO: zero_write(a & x); break; // Unofficial
        case STA_ZERO: zero_write(a);     break;
        case STX_ZERO: zero_write(x);     break;
        case STY_ZERO: zero_write(y);     break;

        // Unofficial NOPs with zero page addressing (acts like reads)
        case NO0_ZERO: case NO1_ZERO: case NO2_ZERO: get_zero_op(); break;

        //
        // Zero page indexed addressing
        //

        // Read instructions

        case ADC_ZERO_X: adc(get_zero_xy_op(x));     break;
        case AND_ZERO_X: and_(get_zero_xy_op(x));    break;
        case CMP_ZERO_X: comp(a, get_zero_xy_op(x)); break;
        case EOR_ZERO_X: eor(get_zero_xy_op(x));     break;
        case LAX_ZERO_Y: lax(get_zero_xy_op(y));     break; // Unofficial
        case LDA_ZERO_X: lda(get_zero_xy_op(x));     break;
        case LDX_ZERO_Y: ldx(get_zero_xy_op(y));     break;
        case LDY_ZERO_X: ldy(get_zero_xy_op(x));     break;
        case ORA_ZERO_X: ora(get_zero_xy_op(x));     break;
        case SBC_ZERO_X: sbc(get_zero_xy_op(x));     break;

        // Read-modify-write instructions

        case ASL_ZERO_X: ZERO_X_RMW(asl); break;
        case DCP_ZERO_X: ZERO_X_RMW(dcp); break; // Unofficial
        case DEC_ZERO_X: ZERO_X_RMW(dec); break;
        case INC_ZERO_X: ZERO_X_RMW(inc); break;
        case ISC_ZERO_X: ZERO_X_RMW(isc); break; // Unofficial
        case LSR_ZERO_X: ZERO_X_RMW(lsr); break;
        case RLA_ZERO_X: ZERO_X_RMW(rla); break; // Unofficial
        case RRA_ZERO_X: ZERO_X_RMW(rra); break; // Unofficial
        case ROL_ZERO_X: ZERO_X_RMW(rol); break;
        case ROR_ZERO_X: ZERO_X_RMW(ror); break;
        case SLO_ZERO_X: ZERO_X_RMW(slo); break; // Unofficial
        case SRE_ZERO_X: ZERO_X_RMW(sre); break; // Unofficial

        // Write instructions

        case SAX_ZERO_Y: zero_xy_write(a & x, y); break; // Unofficial
        case STA_ZERO_X: zero_xy_write(a, x);     break;
        case STX_ZERO_Y: zero_xy_write(x, y);     break;
        case STY_ZERO_X: zero_xy_write(y, x);     break;

        // Unofficial NOPs with indexed zero page addressing (acts like reads)
        case NO0_ZERO_X: case NO1_ZERO_X: case NO2_ZERO_X: case NO3_ZERO_X: case NO4_ZERO_X:
        case NO5_ZERO_X:
            get_zero_xy_op(x);
            break;

        //
        // Absolute indexed addressing
        //

        // Read instructions

        case ADC_ABS_X: adc(get_abs_xy_op_read(x));     break;
        case ADC_ABS_Y: adc(get_abs_xy_op_read(y));     break;
        case AND_ABS_X: and_(get_abs_xy_op_read(x));    break;
        case AND_ABS_Y: and_(get_abs_xy_op_read(y));    break;
        case CMP_ABS_X: comp(a, get_abs_xy_op_read(x)); break;
        case CMP_ABS_Y: comp(a, get_abs_xy_op_read(y)); break;
        case EOR_ABS_X: eor(get_abs_xy_op_read(x));     break;
        case EOR_ABS_Y: eor(get_abs_xy_op_read(y));     break;
        case LAS_ABS_Y: las(get_abs_xy_op_read(y));     break; // Unofficial
        case LAX_ABS_Y: lax(get_abs_xy_op_read(y));     break; // Unofficial
        case LDA_ABS_X: lda(get_abs_xy_op_read(x));     break;
        case LDA_ABS_Y: lda(get_abs_xy_op_read(y));     break;
        case LDX_ABS_Y: ldx(get_abs_xy_op_read(y));     break;
        case LDY_ABS_X: ldy(get_abs_xy_op_read(x));     break;
        case ORA_ABS_X: ora(get_abs_xy_op_read(x));     break;
        case ORA_ABS_Y: ora(get_abs_xy_op_read(y));     break;
        case SBC_ABS_X: sbc(get_abs_xy_op_read(x));     break;
        case SBC_ABS_Y: sbc(get_abs_xy_op_read(y));     break;

        // Read-modify-write instructions

        case ASL_ABS_X: RMW(asl, get_abs_xy_addr_write(x)); break;
        case DCP_ABS_X: RMW(dcp, get_abs_xy_addr_write(x)); break; // Unofficial
        case DCP_ABS_Y: RMW(dcp, get_abs_xy_addr_write(y)); break; // Unofficial
        case DEC_ABS_X: RMW(dec, get_abs_xy_addr_write(x)); break;
        case INC_ABS_X: RMW(inc, get_abs_xy_addr_write(x)); break;
        case ISC_ABS_X: RMW(isc, get_abs_xy_addr_write(x)); break; // Unofficial
        case ISC_ABS_Y: RMW(isc, get_abs_xy_addr_write(y)); break; // Unofficial
        case LSR_ABS_X: RMW(lsr, get_abs_xy_addr_write(x)); break;
        case RLA_ABS_X: RMW(rla, get_abs_xy_addr_write(x)); break; // Unofficial
        case RLA_ABS_Y: RMW(rla, get_abs_xy_addr_write(y)); break; // Unofficial
        case RRA_ABS_X: RMW(rra, get_abs_xy_addr_write(x)); break; // Unofficial
        case RRA_ABS_Y: RMW(rra, get_abs_xy_addr_write(y)); break; // Unofficial
        case ROL_ABS_X: RMW(rol, get_abs_xy_addr_write(x)); break;
        case ROR_ABS_X: RMW(ror, get_abs_xy_addr_write(x)); break;
        case SLO_ABS_X: RMW(slo, get_abs_xy_addr_write(x)); break; // Unofficial
        case SLO_ABS_Y: RMW(slo, get_abs_xy_addr_write(y)); break; // Unofficial
        case SRE_ABS_X: RMW(sre, get_abs_xy_addr_write(x)); break; // Unofficial
        case SRE_ABS_Y: RMW(sre, get_abs_xy_addr_write(y)); break; // Unofficial

        // Write instructions

        case AXA_ABS_Y: unoff_addr_write(get_abs_addr(), a & x, y); break; // Unofficial
        case SAY_ABS_X: unoff_addr_write(get_abs_addr(), y    , x); break; // Unofficial
        case XAS_ABS_Y: unoff_addr_write(get_abs_addr(), x    , y); break; // Unofficial
        // Unofficial
        case TAS_ABS_Y:
            s = a & x;
            unoff_addr_write(get_abs_addr(), a & x, y);
            break;

        case STA_ABS_X: abs_xy_write_a(x); break;
        case STA_ABS_Y: abs_xy_write_a(y); break;

        // Unofficial NOPs with absolute,x addressing (acts like reads)
        case NO0_ABS_X: case NO1_ABS_X: case NO2_ABS_X: case NO3_ABS_X: case NO4_ABS_X:
        case NO5_ABS_X:
            get_abs_xy_op_read(x);
            break;

        //
        // Indexed indirect addressing
        //

        // Read instructions

        case ADC_IND_X: adc(get_ind_x_op());     break;
        case AND_IND_X: and_(get_ind_x_op());    break;
        case CMP_IND_X: comp(a, get_ind_x_op()); break;
        case EOR_IND_X: eor(get_ind_x_op());     break;
        case LAX_IND_X: lax(get_ind_x_op());     break; // Unofficial
        case LDA_IND_X: lda(get_ind_x_op());     break;
        case ORA_IND_X: ora(get_ind_x_op());     break;
        case SBC_IND_X: sbc(get_ind_x_op());     break;

        // Write instructions

        case SAX_IND_X: ind_x_write(a & x); break; // Unofficial
        case STA_IND_X: ind_x_write(a);     break;

        // Read-modify-write instructions

        case DCP_IND_X: RMW(dcp, get_ind_x_addr()); break; // Unofficial
        case ISC_IND_X: RMW(isc, get_ind_x_addr()); break; // Unofficial
        case RLA_IND_X: RMW(rla, get_ind_x_addr()); break; // Unofficial
        case RRA_IND_X: RMW(rra, get_ind_x_addr()); break; // Unofficial
        case SLO_IND_X: RMW(slo, get_ind_x_addr()); break; // Unofficial
        case SRE_IND_X: RMW(sre, get_ind_x_addr()); break; // Unofficial

        //
        // Indirect indexed addressing
        //

        // Read instructions

        case ADC_IND_Y: adc(get_ind_y_op_read());     break;
        case AND_IND_Y: and_(get_ind_y_op_read());    break;
        case CMP_IND_Y: comp(a, get_ind_y_op_read()); break;
        case EOR_IND_Y: eor(get_ind_y_op_read());     break;
        case LAX_IND_Y: lax(get_ind_y_op_read());     break; // Unofficial
        case LDA_IND_Y: lda(get_ind_y_op_read());     break;
        case ORA_IND_Y: ora(get_ind_y_op_read());     break;
        case SBC_IND_Y: sbc(get_ind_y_op_read());     break;

        // Write instructions

        // Unofficial
        case AXA_IND_Y:
        {
            ++pc;
            read_tick(); // Fetch effective address low
            read_tick(); // Fetch effective address high
            unoff_addr_write(
              (ram[(op_1 + 1) & 0xFF] << 8) | ram[op_1], // Address
              a & x, y);
            break;
        }

        case STA_IND_Y: ind_y_write_a(); break;

        // Read-modify-write instructions

        case DCP_IND_Y: RMW(dcp, get_ind_y_addr_write()); break; // Unofficial
        case ISC_IND_Y: RMW(isc, get_ind_y_addr_write()); break; // Unofficial
        case RLA_IND_Y: RMW(rla, get_ind_y_addr_write()); break; // Unofficial
        case RRA_IND_Y: RMW(rra, get_ind_y_addr_write()); break; // Unofficial
        case SLO_IND_Y: RMW(slo, get_ind_y_addr_write()); break; // Unofficial
        case SRE_IND_Y: RMW(sre, get_ind_y_addr_write()); break; // Unofficial

        //
        // Indirect addressing
        //

        case JMP_IND:
            {
            uint16_t const addr = (read(pc + 1) << 8) | op_1;
            pc = read(addr);
            poll_for_interrupt();
            pc |= (read((addr & 0xFF00) | ((addr + 1) & 0xFF)) << 8);
            break;
            }

        //
        // Branch instructions
        //

        case BCC: branch_if(!carry_flag);                   break;
        case BCS: branch_if(carry_flag);                    break;
        case BVC: branch_if(!overflow_flag);                break;
        case BVS: branch_if(overflow_flag);                 break;
        case BEQ: branch_if(!(zero_negative_flag & 0xFF));  break;
        case BMI: branch_if(zero_negative_flag & 0x180);    break;
        case BNE: branch_if(zero_negative_flag & 0xFF);     break;
        case BPL: branch_if(!(zero_negative_flag & 0x180)); break;

        //
        // KIL instructions (hang the CPU)
        //

        case KI0: case KI1: case KI2: case KI3: case KI4: case KI5:
        case KI6: case KI7: case KI8: case KI9: case K10: case K11:
            puts("KIL instruction executed, system hung");
            exit(0);
        }
    }
}

// }}}

// Tracing and logging {{{

#ifdef LOG_INSTRUCTIONS

static int read_without_side_effects(uint16_t addr) {
    switch (pc) {
    case 0x0000 ... 0x1FFF: return ram[addr & 0x07FF];
    case 0x6000 ... 0x7FFF: return prg_ram ? prg_ram[addr & 0x1FFF] : 0;
    case 0x8000 ... 0xFFFF: return PRG(addr);

    default: return -1;
    }
}

static char const*decode_addr(uint16_t addr) {
    static char addr_str[64];

    static char const*const desc_2000_regs[]
      = { "PPUCTRL", "PPUMASK"  , "PPUSTATUS", "OAMADDR",
          "OAMDATA", "PPUSCROLL", "PPUADDR"  , "PPUDATA" };

    static char const*const desc_4000_regs[]
      = { // $4000-$4007
          "Pulse 1 duty, loop, and volume", "Pulse 1 sweep unit"           ,
          "Pulse 1 timer low"             , "Pulse 1 length and timer high",
          "Pulse 2 duty, loop, and volume", "Pulse 2 sweep unit"           ,
          "Pulse 2 timer low"             , "Pulse 2 length and timer high",

          // $4008-$400B
          "Triangle linear length", 0, "Triangle timer low", "Triangle length and timer high",

          // $400C-$400F
          "Noise volume", 0, "Noise loop and period", "Noise length",

          // $4010-$4013
          "DMC IRQ, loop, and frequency", "DMC counter"      ,
          "DMC sample address"          , "DMC sample length",

          // $4014-$4017
          "OAM DMA", "APU status", "Read controller 1",
          "Frame counter and read controller 2" };

    char const*addr_desc;
    if (addr >= 0x2000 && addr <= 0x3FFF)
        addr_desc = desc_2000_regs[addr & 7];
    else if (addr >= 0x4000 && addr <= 0x4017)
        addr_desc = desc_4000_regs[addr - 0x4000];
    else
        addr_desc = 0;

    if (addr_desc)
        sprintf(addr_str, "$%04X (%s)", addr, addr_desc);
    else
        sprintf(addr_str, "$%04X", addr);
    return addr_str;
}

// TODO: Tableify

#define INS_IMP(name)    case name         : fputs (#name"         ", stdout);                            break;
#define INS_ACC(name)    case name##_ACC   : fputs (#name" A       ", stdout);                            break;
#define INS_IMM(name)    case name##_IMM   : printf(#name" #$%02X    ", op_1);                            break;
#define INS_ZERO(name)   case name##_ZERO  : printf(#name" $%02X     ", op_1);                            break;
#define INS_ZERO_X(name) case name##_ZERO_X: printf(#name" $%02X,X   ", op_1);                            break;
#define INS_ZERO_Y(name) case name##_ZERO_Y: printf(#name" $%02X,Y   ", op_1);                            break;
#define INS_REL(name)    case name         : printf(#name" $%04X   "  , uint16_t(pc + 2 + (int8_t)op_1)); break;
#define INS_IND_X(name)  case name##_IND_X : printf(#name" ($%02X,X) ", op_1);                            break;
#define INS_IND_Y(name)  case name##_IND_Y : printf(#name" ($%02X),Y ", op_1);                            break;
#define INS_ABS(name)    case name##_ABS   : printf(#name" %s   "  , decode_addr((op_2 << 8) | op_1));    break;
#define INS_ABS_X(name)  case name##_ABS_X : printf(#name" %s,X "  , decode_addr((op_2 << 8) | op_1));    break;
#define INS_ABS_Y(name)  case name##_ABS_Y : printf(#name" %s,Y "  , decode_addr((op_2 << 8) | op_1));    break;
#define INS_IND(name)    case name##_IND   : printf(#name" (%s) "  , decode_addr((op_2 << 8) | op_1));    break;

static bool breakpoint_at[0x10000];
// Optimization to avoid trashing the cache via breakpoint_at lookups when no
// breakpoints are set
static unsigned n_breakpoints_set;

static enum { RUN, SINGLE_STEP, TRACE } debug_mode;// = SINGLE_STEP;

static void print_instruction(uint16_t addr) {
    int opcode, op_1, op_2;

    printf("%04X: ", addr);

    if ((opcode = read_without_side_effects(addr)) == -1) {
        puts("(strange address while reading opcode - skipping)");
        return;
    }

    switch (opcode) {
    // Implied
    INS_IMP(BRK) INS_IMP(RTI) INS_IMP(RTS) INS_IMP(PHA) INS_IMP(PHP)
    INS_IMP(PLA) INS_IMP(PLP) INS_IMP(CLC) INS_IMP(CLD) INS_IMP(CLI)
    INS_IMP(CLV) INS_IMP(SEC) INS_IMP(SED) INS_IMP(SEI) INS_IMP(DEX)
    INS_IMP(DEY) INS_IMP(INX) INS_IMP(INY) INS_IMP(NO0) INS_IMP(NO1)
    INS_IMP(NO2) INS_IMP(NO3) INS_IMP(NO4) INS_IMP(NO5) INS_IMP(NOP)
    INS_IMP(TAX) INS_IMP(TAY) INS_IMP(TSX) INS_IMP(TXA) INS_IMP(TXS)
    INS_IMP(TYA)

    // KIL instructions (implied)
    INS_IMP(KI0) INS_IMP(KI1) INS_IMP(KI2) INS_IMP(KI3) INS_IMP(KI4)
    INS_IMP(KI5) INS_IMP(KI6) INS_IMP(KI7) INS_IMP(KI8) INS_IMP(KI9)
    INS_IMP(K10) INS_IMP(K11)

    // Accumulator
    INS_ACC(ASL) INS_ACC(LSR) INS_ACC(ROL) INS_ACC(ROR)

    default: goto needs_first_operand;
    }
    return;

needs_first_operand:
    if ((op_1 = read_without_side_effects(addr + 1)) == -1) {
        puts("(strange address while reading first operand byte - skipping)");
        return;
    }

    switch (opcode) {
    // Immediate
    INS_IMM(ADC) INS_IMM(ALR) INS_IMM(AN0) INS_IMM(AN1) INS_IMM(AND)
    INS_IMM(ARR) INS_IMM(AXS) INS_IMM(ATX) INS_IMM(CMP) INS_IMM(CPX)
    INS_IMM(CPY) INS_IMM(EOR) INS_IMM(LDA) INS_IMM(LDX) INS_IMM(LDY)
    INS_IMM(NO0) INS_IMM(NO1) INS_IMM(NO2) INS_IMM(NO3) INS_IMM(NO4)
    INS_IMM(ORA) INS_IMM(SB2) INS_IMM(SBC) INS_IMM(XAA)

    // Zero page
    INS_ZERO(ADC) INS_ZERO(AND) INS_ZERO(BIT) INS_ZERO(CMP)
    INS_ZERO(CPX) INS_ZERO(CPY) INS_ZERO(DCP) INS_ZERO(EOR)
    INS_ZERO(ISC) INS_ZERO(LAX) INS_ZERO(LDA) INS_ZERO(LDX)
    INS_ZERO(LDY) INS_ZERO(NO0) INS_ZERO(NO1) INS_ZERO(NO2)
    INS_ZERO(ORA) INS_ZERO(SBC) INS_ZERO(SLO) INS_ZERO(SRE)
    INS_ZERO(SAX) INS_ZERO(ASL) INS_ZERO(LSR) INS_ZERO(RLA)
    INS_ZERO(RRA) INS_ZERO(ROL) INS_ZERO(ROR) INS_ZERO(INC)
    INS_ZERO(DEC) INS_ZERO(STA) INS_ZERO(STX) INS_ZERO(STY)

    // Zero page, X
    INS_ZERO_X(ADC) INS_ZERO_X(AND) INS_ZERO_X(CMP) INS_ZERO_X(DCP)
    INS_ZERO_X(EOR) INS_ZERO_X(ISC) INS_ZERO_X(LDA) INS_ZERO_X(LDY)
    INS_ZERO_X(NO0) INS_ZERO_X(NO1) INS_ZERO_X(NO2) INS_ZERO_X(NO3)
    INS_ZERO_X(NO4) INS_ZERO_X(NO5) INS_ZERO_X(ORA) INS_ZERO_X(SBC)
    INS_ZERO_X(SLO) INS_ZERO_X(SRE) INS_ZERO_X(ASL) INS_ZERO_X(DEC)
    INS_ZERO_X(INC) INS_ZERO_X(LSR) INS_ZERO_X(RLA) INS_ZERO_X(RRA)
    INS_ZERO_X(ROL) INS_ZERO_X(ROR) INS_ZERO_X(STA) INS_ZERO_X(STY)

    // Zero page, Y
    INS_ZERO_Y(SAX) INS_ZERO_Y(LAX) INS_ZERO_Y(LDX) INS_ZERO_Y(STX)

    // Relative (branch instructions)
    INS_REL(BCC) INS_REL(BCS) INS_REL(BVC) INS_REL(BVS) INS_REL(BEQ)
    INS_REL(BMI) INS_REL(BNE) INS_REL(BPL)

    // (Indirect,X)
    INS_IND_X(ADC) INS_IND_X(AND) INS_IND_X(CMP) INS_IND_X(DCP)
    INS_IND_X(EOR) INS_IND_X(ISC) INS_IND_X(LAX) INS_IND_X(LDA)
    INS_IND_X(ORA) INS_IND_X(RLA) INS_IND_X(RRA) INS_IND_X(SAX)
    INS_IND_X(SBC) INS_IND_X(SLO) INS_IND_X(SRE) INS_IND_X(STA)

    // (Indirect),Y
    INS_IND_Y(ADC) INS_IND_Y(AND) INS_IND_Y(AXA) INS_IND_Y(CMP)
    INS_IND_Y(DCP) INS_IND_Y(EOR) INS_IND_Y(ISC) INS_IND_Y(LAX)
    INS_IND_Y(LDA) INS_IND_Y(ORA) INS_IND_Y(RLA) INS_IND_Y(RRA)
    INS_IND_Y(SBC) INS_IND_Y(SLO) INS_IND_Y(SRE) INS_IND_Y(STA)

    default: goto needs_second_operand;
    }
    return;

needs_second_operand:
    if ((op_2 = read_without_side_effects(addr + 2)) == -1) {
        puts("(strange address while reading second operand byte - skipping)");
        return;
    }

    switch (opcode) {
    // Absolute
    INS_ABS(JMP) INS_ABS(JSR) INS_ABS(ADC) INS_ABS(AND) INS_ABS(BIT)
    INS_ABS(CMP) INS_ABS(CPX) INS_ABS(CPY) INS_ABS(DCP) INS_ABS(EOR)
    INS_ABS(ISC) INS_ABS(LAX) INS_ABS(LDA) INS_ABS(LDX) INS_ABS(LDY)
    INS_ABS(NOP) INS_ABS(ORA) INS_ABS(SBC) INS_ABS(SLO) INS_ABS(SRE)
    INS_ABS(ASL) INS_ABS(DEC) INS_ABS(INC) INS_ABS(LSR) INS_ABS(RLA)
    INS_ABS(RRA) INS_ABS(ROL) INS_ABS(ROR) INS_ABS(SAX) INS_ABS(STA)
    INS_ABS(STX) INS_ABS(STY)

    // Absolute,X
    INS_ABS_X(ADC) INS_ABS_X(AND) INS_ABS_X(CMP) INS_ABS_X(DCP)
    INS_ABS_X(EOR) INS_ABS_X(ISC) INS_ABS_X(LDA) INS_ABS_X(LDY)
    INS_ABS_X(NO0) INS_ABS_X(NO1) INS_ABS_X(NO2) INS_ABS_X(NO3)
    INS_ABS_X(NO4) INS_ABS_X(NO5) INS_ABS_X(ORA) INS_ABS_X(RLA)
    INS_ABS_X(RRA) INS_ABS_X(SAY) INS_ABS_X(SBC) INS_ABS_X(SLO)
    INS_ABS_X(SRE) INS_ABS_X(ASL) INS_ABS_X(DEC) INS_ABS_X(INC)
    INS_ABS_X(LSR) INS_ABS_X(ROL) INS_ABS_X(ROR) INS_ABS_X(STA)

    // Absolute,Y
    INS_ABS_Y(ADC) INS_ABS_Y(AND) INS_ABS_Y(AXA) INS_ABS_Y(CMP)
    INS_ABS_Y(DCP) INS_ABS_Y(EOR) INS_ABS_Y(ISC) INS_ABS_Y(LAX)
    INS_ABS_Y(LAS) INS_ABS_Y(LDA) INS_ABS_Y(LDX) INS_ABS_Y(ORA)
    INS_ABS_Y(RLA) INS_ABS_Y(RRA) INS_ABS_Y(SBC) INS_ABS_Y(SLO)
    INS_ABS_Y(SRE) INS_ABS_Y(STA) INS_ABS_Y(TAS) INS_ABS_Y(XAS)

    // Indirect
    INS_IND(JMP)

    default: UNREACHABLE
    }
}

static void print_state() {
    printf("A: %02X  X: %02X  Y: %02X  S: %02X  "
           "Carry: %d  Zero: %d  I disable: %d  Decimal: %d  Overflow: %d  Negative: %d  (%u,%u) PPU cycle: %"PRIu64" apu_clk1: %s",
           a, x, y, s,
           carry_flag, !(zero_negative_flag & 0xFF), irq_disable_flag, decimal_flag,
           overflow_flag, !!(zero_negative_flag & 0x180), scanline, dot, ppu_cycle,
           apu_clk1_is_high ? "high" : "low");
}

static void log_instruction() {
    if (debug_mode == RUN) {
        if ((n_breakpoints_set > 0 && breakpoint_at[pc]) || keys[SDLK_d])
            debug_mode = SINGLE_STEP;
        else
            return;
    }

    if (debug_mode == SINGLE_STEP || debug_mode == TRACE) {
        print_instruction(pc);
        print_state();

        if (pending_nmi && pending_irq)
            puts(" (pending NMI and IRQ)");
        else if (pending_nmi)
            puts(" (pending NMI)");
        else if (pending_irq)
            puts(" (pending IRQ)");
        else
            putchar('\n');

        if (debug_mode == TRACE) return;
    }

    // Simple debugger with breakpoints. Only for internal use - does not do
    // robust argument checking.

    static char const delims[] = " \f\t\v";

    for (;;) {
        char *const line = readline("Debug: ");
        if (line) {
            if (!*line) return;

            add_history(line);

            char *const keyword = strtok(line, delims);
            if (keyword) {
                char *const arg = strtok(0, delims);
                switch (*keyword) {
                case 'b':
                {
                    if (!arg) {
                        puts("Missing address");
                        break;
                    }
                    unsigned addr;
                    sscanf(arg, "%x", &addr);
                    if (addr > 0xFFFF)
                        puts("Address out of range");
                    else {
                        // Increment if setting and cleared
                        n_breakpoints_set += !breakpoint_at[addr];
                        breakpoint_at[addr] = true;
                    }
                    break;
                }

                case 'c':
                    debug_mode = RUN;
                    free(line);
                    sync_input();
                    return;

                case 'd':
                {
                    if (!arg) {
                        puts("Missing address");
                        break;
                    }
                    unsigned addr;
                    sscanf(arg, "%x", &addr);
                    if (addr > 0xFFFF)
                        puts("Address out of range");
                    else {
                        // Decrement if clearing and set
                        n_breakpoints_set -= breakpoint_at[addr];
                        if (!breakpoint_at[addr])
                            puts("No breakpoint at address");
                        breakpoint_at[addr] = false;
                    }
                    break;
                }

                case 'D':
                    for (unsigned i = 0; i < ARRAY_LEN(breakpoint_at); ++i) {
                        if (breakpoint_at[i]) {
                            printf("Deleted breakpoint at %04X\n", i);
                            breakpoint_at[i] = false;
                        }
                    }
                    n_breakpoints_set = 0;
                    break;

                case 'i':
                    for (unsigned i = 0; i < ARRAY_LEN(breakpoint_at); ++i)
                        if (breakpoint_at[i])
                            printf("Breakpoint at %04X\n", i);
                    break;

                case 'q':
                    exit(0);
                    break;

                default:
                    printf("Unknown command '%c'\n", *keyword);
                    break;
                }
            }
            free(line);
        }
    }
}
#endif // LOG_INSTRUCTION

// }}}

// Initialization and resetting {{{

void init_cpu() { init_interrupts(); }

static void set_cpu_cold_boot_state() {
    init_array(ram, (uint8_t)0xFF);
    cpu_data_bus = 0;

    // s is later decremented to 0xFD during the reset operation
    a = s = x = y = 0;

    zero_negative_flag = 1; // Neither negative nor zero
    overflow_flag      = false;
    decimal_flag       = false;
    irq_disable_flag   = false; // Later set by reset
    carry_flag         = false;

    irq_line     = pending_irq = cart_irq = false;
    nmi_asserted = pending_nmi = false;

    cpu_is_reading = true;

    // TODO: Might be better to do this in conjunction with loading a new ROM
#ifdef LOG_INSTRUCTIONS
    init_array(breakpoint_at, false);
#endif
}

#ifdef RUN_TESTS
static void reset_cpu() {
    irq_line = pending_irq = cart_irq = false;

    // This sets the interrupt flag as a side effect
    do_interrupt(Int_reset);
}
#endif

// }}}
