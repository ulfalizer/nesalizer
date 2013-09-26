#include "common.h"

#include "cpu.h"
#include "mapper.h"
#include "ppu.h"
#include "rom.h"

static unsigned reg_8000;

// regs[0-5] define CHR mappings, regs[6-7] PRG mappings
static unsigned regs[8];

// IRQs

static uint8_t irq_period;
static uint8_t irq_period_cnt;
static bool irq_enabled;

static void make_effective() {
    // Second 8K PRG bank fixed to regs[7]
    set_prg_8k_bank(1, regs[7]);
    if (!(reg_8000 & 0x40)) {
        // [ regs[6] | regs[7] | {-2} | {-1} ]
        set_prg_8k_bank(0, regs[6]);
        set_prg_8k_bank(2, 2*prg_16k_banks - 2);
    }
    else {
        // [ {-2} | regs[7] | regs[6] | {-1} ]
        set_prg_8k_bank(0, 2*prg_16k_banks - 2);
        set_prg_8k_bank(2, regs[6]);
    }

    if (!(reg_8000 & 0x80)) {
        // [ <regs[0]> | <regs[1]> | regs[2..5] ]
        set_chr_2k_bank(0, regs[0] >> 1);
        set_chr_2k_bank(1, regs[1] >> 1);
        for (unsigned i = 0; i < 4; ++i)
            set_chr_1k_bank(4 + i, regs[2 + i]);
    }
    else {
        // [ regs[2..5] | <regs[0]> | <regs[1]> ]
        for (unsigned i = 0; i < 4; ++i)
            set_chr_1k_bank(i, regs[2 + i]);
        set_chr_2k_bank(2, regs[0] >> 1);
        set_chr_2k_bank(3, regs[1] >> 1);
    }
}

void mapper_4_init() {
    init_array(regs, (unsigned)0);
    set_mirroring(HORIZONTAL); // Guess

    // Last PRG 8K page fixed
    set_prg_8k_bank(3, 2*prg_16k_banks - 1);

    make_effective();
}

void mapper_4_write(uint8_t value, uint16_t addr) {
    LOG_MAPPER("MMC3: Writing %02X to $%04X, at (%u,%u)\n", value, addr, scanline, dot);

    switch (((addr >> 12) & 6) | (addr & 1)) {

    case 0: // 0x8000
        LOG_MAPPER("8000\n");
        reg_8000 = value;
        break;

    case 1: // 0x8001
        LOG_MAPPER("8001\n");
        regs[reg_8000 & 7] = value;
        break;

    case 2: // 0xA000 ("Ignore when 4-screen")
        LOG_MAPPER("A000\n");
        set_mirroring(value & 1 ? HORIZONTAL : VERTICAL);
        break;

    case 3: // 0xA001
        LOG_MAPPER("A001\n");
        // WRAM stuff...
        // Is PRG-RAM and WRAM/SRAM always into the same region?
        break;

    case 4: // 0xC000
        LOG_MAPPER("MMC3: Setting IRQ period to %d, at scanline = %u, dot = %u\n", value, scanline, dot);
        irq_period = value;
        break;

    case 5: // 0xC001
        LOG_MAPPER("MMC3: Setting IRQ period counter to 0, at scanline = %u, dot = %u\n", scanline, dot);
        // This causes the period to be reloaded at the next rising edge
        irq_period_cnt = 0;
        break;

    case 6: // 0xE000
        LOG_MAPPER("MMC3: Clearing IRQ enabled flag and IRQ line, at scanline = %u, dot = %u\n", scanline, dot);
        cart_irq = irq_enabled = false;
        update_irq_status();
        break;

    case 7: // 0xE001
        LOG_MAPPER("MMC3: Enabling IRQs, at scanline = %u, dot = %u\n", scanline, dot);
        irq_enabled = true;
        break;

    default: UNREACHABLE
    }

    make_effective();
}

// There is a short delay after A12 rises till IRQ is asserted, but it probably
// only matters for certain CPU/PPU alignments, if ever:
// http://forums.nesdev.com/viewtopic.php?f=3&t=10340&p=115998#p115996

//static unsigned delayed_irq;

static void clock_scanline_counter() {
    // Revision A: assert IRQ when transitioning from non-zero to zero
    // Revision B: assert IRQ when is zero
    // Revision B implemented here
    if (irq_period_cnt == 0) {
        LOG_MAPPER("MMC3: Reloading IRQ period counter with %u, at scanline = %u, dot = %u\n", irq_period, scanline, dot);
        irq_period_cnt = irq_period;
    }
    else {
        LOG_MAPPER("MMC3: Decrementing IRQ period counter from %u to %u, at scanline = %u, dot = %u\n", irq_period_cnt, irq_period_cnt - 1, scanline, dot);
        --irq_period_cnt;
    }

    if (irq_period_cnt == 0 && irq_enabled) {
        LOG_MAPPER("MMC3: Asserting IRQ at scanline = %u, dot = %u\n", scanline, dot);
        //delayed_irq = 3;
        cart_irq = true;
        update_irq_status();
    }
}

static uint64_t last_a12_high_cycle;

unsigned const min_a12_rise_diff = 16;

void mmc3_ppu_tick_callback() {
    //if (delayed_irq > 0) {
        //if (--delayed_irq == 0) {
            //cart_irq = true;
            //update_irq_status();
        //}
    //}

    if (ppu_addr_bus & 0x1000) {
        if (ppu_cycle - last_a12_high_cycle >= min_a12_rise_diff)
            clock_scanline_counter();
        last_a12_high_cycle = ppu_cycle;
    }
}
