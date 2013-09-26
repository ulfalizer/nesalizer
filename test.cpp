#include "common.h"

#include "audio.h"
#include "cpu.h"
#include "rom.h"
#include "sdl_backend.h"

bool end_testing;

static char const*filename;

void report_status_and_end_test(uint8_t status, char const *msg) {
    if (status == 0)
        printf("%-60s OK\n", filename);
    else
        printf("%-60s FAILED\nvvv TEST OUTPUT START vvv\n%s\n^^^ TEST OUTPUT END ^^^\n",
               filename, msg);
    end_emulation = true;
}

static void run_test(char const *file) {
    filename = file;
    load_rom(file, false);
    run();
    unload_rom();
    // Hack to prevent the next test from running if the program is closed
    // during testing
    if (end_testing) {
        deinit_audio();
        deinit_sdl();
        exit(0);
    }
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

    run_test("tests/ppu_vbl_nmi/rom_singles/01-vbl_basics.nes");
    run_test("tests/ppu_vbl_nmi/rom_singles/02-vbl_set_time.nes");
    run_test("tests/ppu_vbl_nmi/rom_singles/03-vbl_clear_time.nes");
    run_test("tests/ppu_vbl_nmi/rom_singles/04-nmi_control.nes");
    run_test("tests/ppu_vbl_nmi/rom_singles/05-nmi_timing.nes");
    run_test("tests/ppu_vbl_nmi/rom_singles/06-suppression.nes");
    // TODO: Investigate. Possibly depends on analog effects.
    //run_test("tests/ppu_vbl_nmi/rom_singles/07-nmi_on_timing.nes");
    run_test("tests/ppu_vbl_nmi/rom_singles/08-nmi_off_timing.nes");
    run_test("tests/ppu_vbl_nmi/rom_singles/09-even_odd_frames.nes");
    // TODO: Has tricky timing
    //run_test("tests/ppu_vbl_nmi/rom_singles/10-even_odd_timing.nes");

    putchar('\n');

    run_test("tests/ppu_open_bus/ppu_open_bus.nes");

    putchar('\n');

    run_test("tests/apu_test/rom_singles/1-len_ctr.nes");
    run_test("tests/apu_test/rom_singles/2-len_table.nes");
    run_test("tests/apu_test/rom_singles/3-irq_flag.nes");
    run_test("tests/apu_test/rom_singles/4-jitter.nes");
    run_test("tests/apu_test/rom_singles/5-len_timing.nes");
    run_test("tests/apu_test/rom_singles/6-irq_flag_timing.nes");
    run_test("tests/apu_test/rom_singles/7-dmc_basics.nes");
    run_test("tests/apu_test/rom_singles/8-dmc_rates.nes");

    putchar('\n');

    run_test("tests/sprdma_and_dmc_dma/sprdma_and_dmc_dma.nes");
    run_test("tests/sprdma_and_dmc_dma/sprdma_and_dmc_dma_512.nes");

    putchar('\n');

    run_test("tests/apu_reset/4015_cleared.nes");
    run_test("tests/apu_reset/4017_timing.nes");
    run_test("tests/apu_reset/4017_written.nes");
    run_test("tests/apu_reset/irq_flag_cleared.nes");
    run_test("tests/apu_reset/len_ctrs_enabled.nes");
    run_test("tests/apu_reset/works_immediately.nes");

    putchar('\n');

    run_test("tests/mmc3_test_2/rom_singles/1-clocking.nes");
    run_test("tests/mmc3_test_2/rom_singles/2-details.nes");
    run_test("tests/mmc3_test_2/rom_singles/3-A12_clocking.nes");
    run_test("tests/mmc3_test_2/rom_singles/4-scanline_timing.nes");
    run_test("tests/mmc3_test_2/rom_singles/5-MMC3.nes");
    // Old-style behavior. Not yet implemented.
    //run_test("tests/mmc3_test_2/rom_singles/6-MMC3_alt.nes");

    putchar('\n');

    run_test("tests/oam_read/oam_read.nes");

    putchar('\n');

    run_test("tests/oam_stress/oam_stress.nes");

    putchar('\n');

    run_test("tests/cpu_reset/ram_after_reset.nes");
    run_test("tests/cpu_reset/registers.nes");

    putchar('\n');

    run_test("tests/instr_test-v4/rom_singles/01-basics.nes");
    run_test("tests/instr_test-v4/rom_singles/02-implied.nes");
    run_test("tests/instr_test-v4/rom_singles/03-immediate.nes");
    run_test("tests/instr_test-v4/rom_singles/04-zero_page.nes");
    run_test("tests/instr_test-v4/rom_singles/05-zp_xy.nes");
    run_test("tests/instr_test-v4/rom_singles/06-absolute.nes");
    run_test("tests/instr_test-v4/rom_singles/07-abs_xy.nes");
    run_test("tests/instr_test-v4/rom_singles/08-ind_x.nes");
    run_test("tests/instr_test-v4/rom_singles/09-ind_y.nes");
    run_test("tests/instr_test-v4/rom_singles/10-branches.nes");
    run_test("tests/instr_test-v4/rom_singles/11-stack.nes");
    run_test("tests/instr_test-v4/rom_singles/12-jmp_jsr.nes");
    run_test("tests/instr_test-v4/rom_singles/13-rts.nes");
    run_test("tests/instr_test-v4/rom_singles/14-rti.nes");
    run_test("tests/instr_test-v4/rom_singles/15-brk.nes");
    run_test("tests/instr_test-v4/rom_singles/16-special.nes");

    putchar('\n');

    run_test("tests/instr_misc/rom_singles/01-abs_x_wrap.nes");
    run_test("tests/instr_misc/rom_singles/02-branch_wrap.nes");
    run_test("tests/instr_misc/rom_singles/03-dummy_reads.nes");
    run_test("tests/instr_misc/rom_singles/04-dummy_reads_apu.nes");

    putchar('\n');

    run_test("tests/cpu_interrupts_v2/rom_singles/1-cli_latency.nes");
    run_test("tests/cpu_interrupts_v2/rom_singles/2-nmi_and_brk.nes");
    run_test("tests/cpu_interrupts_v2/rom_singles/3-nmi_and_irq.nes");
    run_test("tests/cpu_interrupts_v2/rom_singles/4-irq_and_dma.nes");
    run_test("tests/cpu_interrupts_v2/rom_singles/5-branch_delays_irq.nes");

    putchar('\n');

    run_test("tests/instr_timing/rom_singles/1-instr_timing.nes");
    run_test("tests/instr_timing/rom_singles/2-branch_timing.nes");

    putchar('\n');
}
