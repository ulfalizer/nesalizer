// Holds initialized data where C99's designated initializer feature is handy

#include "opcodes.h"

#include <stdint.h>

// See cpu.cpp.
//
// For CLI and SEI, the flag changes after the interrupt is polled, so both
// can safely go in this list (the polling is influenced by the current
// flag status like for the rest)
uint8_t const polls_irq_after_first_cycle[256] = {
    [ADC_IMM] = 1, [ALR_IMM] = 1, [AN0_IMM] = 1, [AN1_IMM] = 1, [AND_IMM] = 1, [ARR_IMM] = 1, [ASL_ACC] = 1,
    [BCC]     = 1, [ATX_IMM] = 1, [AXS_IMM] = 1, [BCS]     = 1, [BEQ]     = 1, [BMI]     = 1, [BNE]     = 1,
    [BPL]     = 1, [BVC]     = 1, [BVS]     = 1, [CLC]     = 1, [CLD]     = 1, [CLI]     = 1, [CLV]     = 1,
    [CMP_IMM] = 1, [CPX_IMM] = 1, [CPY_IMM] = 1, [DEX]     = 1, [DEY]     = 1, [EOR_IMM] = 1, [INX]     = 1,
    [INY]     = 1, [LDA_IMM] = 1, [LDX_IMM] = 1, [LDY_IMM] = 1, [LSR_ACC] = 1, [NO0]     = 1, [NO0_IMM] = 1,
    [NO1_IMM] = 1, [NO2_IMM] = 1, [NO3_IMM] = 1, [NO4_IMM] = 1, [NO1]     = 1, [NO2]     = 1, [NO3]     = 1,
    [NO4]     = 1, [NO5]     = 1, [NOP]     = 1, [ORA_IMM] = 1, [ROL_ACC] = 1, [ROR_ACC] = 1, [SB2_IMM] = 1,
    [SBC_IMM] = 1, [SEC]     = 1, [SED]     = 1, [SEI]     = 1, [TAX]     = 1, [TAY]     = 1, [TSX]     = 1,
    [TXA]     = 1, [TXS]     = 1, [TYA]     = 1, [XAA_IMM] = 1 };
