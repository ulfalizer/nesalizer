#include "common.h"

#include "cpu.h"
#include "mapper.h"
#include "rom.h"
#include "sdl_backend.h"

bool end_testing;

static char const *filename;

void report_status_and_end_test(uint8_t status, char const *msg) {
    if (status == 0)
        printf("%-60s OK\n", filename);
    else
        printf("%-60s FAILED\nvvv TEST OUTPUT START vvv\n%s\n^^^ TEST OUTPUT END ^^^\n",
               filename, msg);
    end_emulation();
}

static void run_test(char const *file) {
    filename = file;
    load_rom(file, false);
    run();
    unload_rom();
}

void run_tests() {
    // These can't be automated as easily:
    //   cpu_dummy_reads
    //   sprite_hit_tests_2005.10.05
    //   sprite_overflow_tests
    //
    // Tests that require manual inspection:
    //   dpcm_letterbox
    //   nmi_sync
    //
    // Look into these too:
    //   dmc_tests

    // Do it like this to avoid extra newlines being printed when aborting
    // testing
    #define RUN_TEST(file) run_test(file); if (end_testing) goto end;

    RUN_TEST("tests/ppu_vbl_nmi/rom_singles/01-vbl_basics.nes");
    RUN_TEST("tests/ppu_vbl_nmi/rom_singles/02-vbl_set_time.nes");
    RUN_TEST("tests/ppu_vbl_nmi/rom_singles/03-vbl_clear_time.nes");
    RUN_TEST("tests/ppu_vbl_nmi/rom_singles/04-nmi_control.nes");
    RUN_TEST("tests/ppu_vbl_nmi/rom_singles/05-nmi_timing.nes");
    RUN_TEST("tests/ppu_vbl_nmi/rom_singles/06-suppression.nes");
    // TODO: Investigate. Possibly depends on analog effects.
    //RUN_TEST("tests/ppu_vbl_nmi/rom_singles/07-nmi_on_timing.nes");
    RUN_TEST("tests/ppu_vbl_nmi/rom_singles/08-nmi_off_timing.nes");
    RUN_TEST("tests/ppu_vbl_nmi/rom_singles/09-even_odd_frames.nes");
    // TODO: Has tricky timing
    //RUN_TEST("tests/ppu_vbl_nmi/rom_singles/10-even_odd_timing.nes");

    putchar('\n');

    RUN_TEST("tests/ppu_open_bus/ppu_open_bus.nes");

    putchar('\n');

    RUN_TEST("tests/apu_test/rom_singles/1-len_ctr.nes");
    RUN_TEST("tests/apu_test/rom_singles/2-len_table.nes");
    RUN_TEST("tests/apu_test/rom_singles/3-irq_flag.nes");
    RUN_TEST("tests/apu_test/rom_singles/4-jitter.nes");
    RUN_TEST("tests/apu_test/rom_singles/5-len_timing.nes");
    RUN_TEST("tests/apu_test/rom_singles/6-irq_flag_timing.nes");
    RUN_TEST("tests/apu_test/rom_singles/7-dmc_basics.nes");
    RUN_TEST("tests/apu_test/rom_singles/8-dmc_rates.nes");

    putchar('\n');

    RUN_TEST("tests/sprdma_and_dmc_dma/sprdma_and_dmc_dma.nes");
    RUN_TEST("tests/sprdma_and_dmc_dma/sprdma_and_dmc_dma_512.nes");

    putchar('\n');

    RUN_TEST("tests/apu_reset/4015_cleared.nes");
    RUN_TEST("tests/apu_reset/4017_timing.nes");
    RUN_TEST("tests/apu_reset/4017_written.nes");
    RUN_TEST("tests/apu_reset/irq_flag_cleared.nes");
    RUN_TEST("tests/apu_reset/len_ctrs_enabled.nes");
    RUN_TEST("tests/apu_reset/works_immediately.nes");

    putchar('\n');

    RUN_TEST("tests/mmc3_test_2/rom_singles/1-clocking.nes");
    RUN_TEST("tests/mmc3_test_2/rom_singles/2-details.nes");
    RUN_TEST("tests/mmc3_test_2/rom_singles/3-A12_clocking.nes");
    RUN_TEST("tests/mmc3_test_2/rom_singles/4-scanline_timing.nes");
    RUN_TEST("tests/mmc3_test_2/rom_singles/5-MMC3.nes");
    // Old-style behavior. Not yet implemented.
    //RUN_TEST("tests/mmc3_test_2/rom_singles/6-MMC3_alt.nes");

    putchar('\n');

    RUN_TEST("tests/oam_read/oam_read.nes");

    putchar('\n');

    RUN_TEST("tests/oam_stress/oam_stress.nes");

    putchar('\n');

    RUN_TEST("tests/cpu_reset/ram_after_reset.nes");
    RUN_TEST("tests/cpu_reset/registers.nes");

    putchar('\n');

    RUN_TEST("tests/instr_test-v4/rom_singles/01-basics.nes");
    RUN_TEST("tests/instr_test-v4/rom_singles/02-implied.nes");
    RUN_TEST("tests/instr_test-v4/rom_singles/03-immediate.nes");
    RUN_TEST("tests/instr_test-v4/rom_singles/04-zero_page.nes");
    RUN_TEST("tests/instr_test-v4/rom_singles/05-zp_xy.nes");
    RUN_TEST("tests/instr_test-v4/rom_singles/06-absolute.nes");
    RUN_TEST("tests/instr_test-v4/rom_singles/07-abs_xy.nes");
    RUN_TEST("tests/instr_test-v4/rom_singles/08-ind_x.nes");
    RUN_TEST("tests/instr_test-v4/rom_singles/09-ind_y.nes");
    RUN_TEST("tests/instr_test-v4/rom_singles/10-branches.nes");
    RUN_TEST("tests/instr_test-v4/rom_singles/11-stack.nes");
    RUN_TEST("tests/instr_test-v4/rom_singles/12-jmp_jsr.nes");
    RUN_TEST("tests/instr_test-v4/rom_singles/13-rts.nes");
    RUN_TEST("tests/instr_test-v4/rom_singles/14-rti.nes");
    RUN_TEST("tests/instr_test-v4/rom_singles/15-brk.nes");
    RUN_TEST("tests/instr_test-v4/rom_singles/16-special.nes");

    putchar('\n');

    RUN_TEST("tests/instr_misc/rom_singles/01-abs_x_wrap.nes");
    RUN_TEST("tests/instr_misc/rom_singles/02-branch_wrap.nes");
    RUN_TEST("tests/instr_misc/rom_singles/03-dummy_reads.nes");
    RUN_TEST("tests/instr_misc/rom_singles/04-dummy_reads_apu.nes");

    putchar('\n');

    RUN_TEST("tests/cpu_interrupts_v2/rom_singles/1-cli_latency.nes");
    RUN_TEST("tests/cpu_interrupts_v2/rom_singles/2-nmi_and_brk.nes");
    RUN_TEST("tests/cpu_interrupts_v2/rom_singles/3-nmi_and_irq.nes");
    RUN_TEST("tests/cpu_interrupts_v2/rom_singles/4-irq_and_dma.nes");
    RUN_TEST("tests/cpu_interrupts_v2/rom_singles/5-branch_delays_irq.nes");

    putchar('\n');

    RUN_TEST("tests/instr_timing/rom_singles/1-instr_timing.nes");
    RUN_TEST("tests/instr_timing/rom_singles/2-branch_timing.nes");

    putchar('\n');

    #undef RUN_TEST

end:
    exit_sdl_thread();
}
