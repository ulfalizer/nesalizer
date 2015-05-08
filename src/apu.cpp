#include "common.h"

#include "apu.h"
#include "audio.h"
#include "cpu.h"
#include "mapper.h"
#include "ppu.h"
#include "rom.h"

// Clock used by the APU and DMA circuitry, parts of which tick at half the CPU
// frequency. Whether the initial tick is high or low seems to be random. The
// name apu_clk1 is from Visual 2A03.
static bool apu_clk1_is_high;

//
// OAM (sprite data) DMA
//
// Put here since it uses the APU clock and has interactions with DMC DMA

// Current OAM DMA state. Needed to get the timing for APU DMC sample loading
// right (tested by the sprdma_and_dmc_dma tests).
static enum OAM_DMA_state {
    OAM_DMA_IN_PROGRESS = 0,
    OAM_DMA_IN_PROGRESS_3RD_TO_LAST_TICK,
    OAM_DMA_IN_PROGRESS_LAST_TICK,
    OAM_DMA_NOT_IN_PROGRESS
} oam_dma_state;

void do_oam_dma(uint8_t addr) {
    // We get either WDTTT... or WDDTTT... where W is the write cycle, D a
    // dummy cycle, and T a transfer cycle (there's 512 of them). The extra
    // dummy cycle occurs if the write cycle has apu_clk1 low.

    // The current OAM DMA state influences the timing for APU DMC sample
    // loads, so we need to keep track of it

    oam_dma_state = OAM_DMA_IN_PROGRESS;

    // Dummy cycles
    if (!apu_clk1_is_high) tick();
    tick();

    unsigned const start_addr = 0x100*addr;
    for (unsigned i = 0; i < 254; ++i) {
        // Do it like this to get open bus right. Could be that it's not
        // visible in any way though.
        cpu_data_bus = read_mem(start_addr + i);
        tick();
        write_oam_data_reg(cpu_data_bus);
    }

    cpu_data_bus = read_mem(start_addr + 254);
    oam_dma_state = OAM_DMA_IN_PROGRESS_3RD_TO_LAST_TICK;
    tick();
    write_oam_data_reg(cpu_data_bus);
    oam_dma_state = OAM_DMA_IN_PROGRESS;

    cpu_data_bus = read_mem(start_addr + 255);
    oam_dma_state = OAM_DMA_IN_PROGRESS_LAST_TICK;
    tick();
    write_oam_data_reg(cpu_data_bus);

    oam_dma_state = OAM_DMA_NOT_IN_PROGRESS;
}


// Set when the output level of any channel changes. Lets us skip the mixing
// step most of the time.
static bool channel_updated;

void begin_audio_frame() { channel_updated = true; }

// Length counter look-up table
uint8_t const len_table[] = {
  10, 254, 20,  2, 40,  4, 80,  6, 160,  8, 60, 10, 14, 12, 26, 14,
  12,  16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30 };

//
// Pulse channels
//

static struct Pulse {
    // Range 0-15
    // (Potentially) affected by
    //   - volume updates,
    //   - length counter updates,
    //   - period updates,
    //   - and waveform position updates
    unsigned output_level;

    bool     enabled;

    bool     const_vol;
    unsigned duty;
    unsigned waveform_pos;
    unsigned len_cnt;
    unsigned period;
    unsigned period_cnt;
    bool     sweep_enabled;
    bool     sweep_negate;
    unsigned sweep_period;
    unsigned sweep_period_cnt;
    unsigned sweep_shift;
    bool     sweep_reload_flag;
    unsigned vol;

    unsigned env_div_cnt;
    unsigned env_vol;
    bool     halt_len_loop_env;
    bool     env_start_flag;

    // Recalculated whenever anything happens that might affect the sweep
    // target period. Not sure if this optimization is still worthwhile.
    int sweep_target_period;
} pulse[2];

static void update_sweep_target_period(unsigned n) {
    int addition = pulse[n].period >> pulse[n].sweep_shift;
    // The adder on the first pulse channel is missing the carry in to the
    // first bit for some unknown reason
    if (pulse[n].sweep_negate) addition = (n == 0) ? ~addition : -addition;
    pulse[n].sweep_target_period = (int)pulse[n].period + addition;
}

static void update_pulse_output_level(unsigned n) {
    static uint8_t const pulse_duties[4][8] =
      { { 0, 1, 0, 0, 0, 0, 0, 0 },
        { 0, 1, 1, 0, 0, 0, 0, 0 },
        { 0, 1, 1, 1, 1, 0, 0, 0 },
        { 1, 0, 0, 1, 1, 1, 1, 1 } };

    unsigned const prev_output_level = pulse[n].output_level;

    if (pulse[n].len_cnt == 0                               ||
        pulse[n].period < 8                                 ||
        !pulse_duties[pulse[n].duty][pulse[n].waveform_pos] ||
        pulse[n].sweep_target_period > 0x7FF) {

        pulse[n].output_level = 0;
    }
    else
        pulse[n].output_level =
          pulse[n].const_vol ? pulse[n].vol : pulse[n].env_vol;

    if (pulse[n].output_level != prev_output_level)
        channel_updated = true;
}

void write_pulse_reg_0(unsigned n, uint8_t val) {
    pulse[n].duty              = val >> 6;
    pulse[n].halt_len_loop_env = val & 0x20;
    pulse[n].const_vol         = val & 0x10;
    pulse[n].vol               = val & 0xF;

    update_pulse_output_level(n);
}

void write_pulse_reg_1(unsigned n, uint8_t val) {
    pulse[n].sweep_enabled = val & 0x80;
    pulse[n].sweep_period  = (val >> 4) & 7;
    pulse[n].sweep_negate  = val & 8;
    pulse[n].sweep_shift   = val & 7;

    pulse[n].sweep_reload_flag = true;

    update_sweep_target_period(n);
    update_pulse_output_level(n);
}

void write_pulse_reg_2(unsigned n, uint8_t val) {
    pulse[n].period = (pulse[n].period & ~0x0FF) | val;

    update_sweep_target_period(n);
    update_pulse_output_level(n);
}

void write_pulse_reg_3(unsigned n, uint8_t val) {
    if (pulse[n].enabled)
        pulse[n].len_cnt = len_table[val >> 3];
    pulse[n].period = (pulse[n].period & ~0x700) | ((val & 7) << 8);

    // Side effects
    pulse[n].waveform_pos   = 0;
    pulse[n].env_start_flag = true;

    update_sweep_target_period(n);
    update_pulse_output_level(n);
}

static void clock_pulse_generator(unsigned n) {
    assert(n < 2);
    assert(pulse[n].duty < 4);
    assert(pulse[n].waveform_pos < 8);
    pulse[n].waveform_pos = (pulse[n].waveform_pos + 1) % 8;

    update_pulse_output_level(n);
}

//
// Triangle channel
//

// Range 0-15, premultiplied by 3 for mixing. Affected only by waveform
// position updates.
static unsigned tri_output_level;

static bool     tri_enabled;

static unsigned tri_period;
static unsigned tri_period_cnt;

static unsigned tri_waveform_pos;

static unsigned tri_len_cnt;
static bool     tri_halt_flag;

static unsigned tri_lin_cnt_load;
static unsigned tri_lin_cnt;
static bool     tri_lin_cnt_reload_flag;

void write_triangle_reg_0(uint8_t val) {
    tri_halt_flag    = val & 0x80;
    tri_lin_cnt_load = val & 0x7F;
}

void write_triangle_reg_1(uint8_t val) {
    tri_period = (tri_period & ~0x0FF) | val;
}

void write_triangle_reg_2(uint8_t val) {
    tri_lin_cnt_reload_flag = true;
    if (tri_enabled)
        tri_len_cnt = len_table[val >> 3];
    tri_period = (tri_period & ~0x700) | ((val & 7) << 8);
}

// Premultiply by three to save multiplication during mixing
uint8_t const tri_waveform_steps[32] =
  { 3*15, 3*14, 3*13, 3*12, 3*11, 3*10, 3*9, 3*8, 3*7, 3*6,  3*5,  3*4,  3*3,  3*2,  3*1,  3*0,
     3*0,  3*1,  3*2,  3*3,  3*4,  3*5, 3*6, 3*7, 3*8, 3*9, 3*10, 3*11, 3*12, 3*13, 3*14, 3*15 };

static void clock_triangle_generator() {
    if (tri_len_cnt > 0 && tri_lin_cnt > 0 &&
        // Prevent ultrasonic frequencies, which cause pops (very audible for Crashman stage in MM2)
        tri_period > 1 &&
        // Ditto for prolly-too-low-to-be-deliberate frequencies
        tri_period <= 0x7FD)
    {
        unsigned const prev_output_level = tri_output_level;

        tri_waveform_pos = (tri_waveform_pos + 1) % 32;
        tri_output_level = tri_waveform_steps[tri_waveform_pos];

        if (tri_output_level != prev_output_level)
            channel_updated = true;
    }
}

//
// Noise channel
//

// Range 0-15, premultiplied by 2 for mixing. Affected by
//   - volume updates,
//   - Length counter updates,
//   - and shift reg value
static unsigned noise_output_level;

static bool     noise_enabled;

static bool     noise_halt_len_loop_env;
static bool     noise_const_vol;
static unsigned noise_vol;
static unsigned noise_feedback_bit;
static unsigned noise_period;
static unsigned noise_period_cnt;
static unsigned noise_len_cnt;
static unsigned noise_shift_reg;
static bool     noise_env_start_flag;
static unsigned noise_env_vol;
static unsigned noise_env_div_cnt;

static void update_noise_output_level() {
    unsigned const prev_output_level = noise_output_level;

    noise_output_level =
      (noise_len_cnt == 0 || !(noise_shift_reg & 1)) ?
      0 :
      2*(noise_const_vol ? noise_vol : noise_env_vol); // Premultiply by 2

    if (noise_output_level != prev_output_level)
        channel_updated = true;
}

// $400C
void write_noise_reg_0(uint8_t val) {
    noise_halt_len_loop_env = val & 0x20;
    noise_const_vol         = val & 0x10;
    noise_vol               = val & 0x0F;

    update_noise_output_level();
}

uint16_t const ntsc_noise_periods[] =
  { 4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068 };
uint16_t const pal_noise_periods[]  =
  { 4, 8, 14, 30, 60, 88, 118, 148, 188, 236, 354, 472, 708,  944, 1890, 3778 };
static uint16_t const *noise_periods;

// $400E
void write_noise_reg_1(uint8_t val) {
    noise_feedback_bit = (val & 0x80) ? 6 : 1;
    noise_period       = noise_periods[val & 0x0F];
}

// $400F
void write_noise_reg_2(uint8_t val) {
    if (noise_enabled) {
        noise_len_cnt = len_table[val >> 3];
        update_noise_output_level();
    }
    noise_env_start_flag = true;
}

static void clock_noise_generator() {
    // Only the lowest bit from 'feedback' is used
    unsigned const feedback = (noise_shift_reg >> noise_feedback_bit) ^ noise_shift_reg;
    noise_shift_reg = (feedback << 14) | (noise_shift_reg >> 1);
    update_noise_output_level();
}

//
// DMC channel
//

// Range 0-127
// Counter value directly determines output level
static unsigned dmc_counter;

// Set by the last sample byte being loaded, unless inhibited or looping is set
// Cleared by
//  * the reset signal,
//  * writing $4015,
//  * and clearing the IRQ enable flag in $4010
bool            dmc_irq;
// $4010
static bool     dmc_irq_enabled;
static bool     dmc_loop_sample;
static unsigned dmc_period;
static unsigned dmc_period_cnt;

// $4012, missing the implied "| 0x8000" that puts it into ROM
static unsigned dmc_sample_start_addr;
// $4013
static unsigned dmc_sample_len;

static uint8_t  dmc_sample_buffer;
static bool     dmc_sample_buffer_has_data;
static uint8_t  dmc_shift_reg;
static bool     dpcm_active;

// True while a sample byte is being loaded, to prevent recursion in
// load_dmc_sample_byte(). This also mirrors how the hardware behaves.
static bool     dmc_loading_sample_byte;

static unsigned dmc_sample_cur_addr; // 15 bits wide
static unsigned dmc_bytes_remaining;
static unsigned dmc_bits_remaining;

uint16_t const ntsc_dmc_periods[] =
 { 428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106,  84,  72,  54 };
uint16_t const pal_dmc_periods[] =
 { 398, 354, 316, 298, 276, 236, 210, 198, 176, 148, 132, 118,  98,  78,  66,  50 };
static uint16_t const *dmc_periods;

void write_dmc_reg_0(uint8_t val) {
    if (!(dmc_irq_enabled = val & 0x80))
        set_dmc_irq(false);
    dmc_loop_sample = val & 0x40;
    dmc_period      = dmc_periods[val & 0x0F];
}

void write_dmc_reg_1(uint8_t val) {
    unsigned const old_dmc_counter = dmc_counter;

    dmc_counter = val & 0x7F;

    if (dmc_counter != old_dmc_counter)
        channel_updated = true;
}

void write_dmc_reg_2(uint8_t val) {
    dmc_sample_start_addr = 0x4000 | (val << 6);
}

void write_dmc_reg_3(uint8_t val) {
    dmc_sample_len = (val << 4) + 1;
}

static void load_dmc_sample_byte() {
    // Timing: http://forums.nesdev.com/viewtopic.php?p=62690#p62690
    static uint8_t const oam_dma_delay[] =
      { 2,   // OAM_DMA_IN_PROGRESS
        1,   // OAM_DMA_IN_PROGRESS_3RD_TO_LAST_TICK
        3 }; // OAM_DMA_IN_PROGRESS_LAST_TICK

    assert(dmc_bytes_remaining > 0);

    // This can happen if a write to $4015 that enables the channel is
    // immediately followed by a DMC clock. The hardware appears to act the
    // same.
    if (dmc_loading_sample_byte)
        return;

    dmc_sample_buffer = read_prg(dmc_sample_cur_addr);
    // Should do this to be OCD and get open bus rights, but it currently
    // breaks OAM DMA
    // cpu_data_bus = dmc_sample_buffer;

    dmc_loading_sample_byte = true;
    unsigned const delay =
      (oam_dma_state != OAM_DMA_NOT_IN_PROGRESS) ?
        oam_dma_delay[oam_dma_state] :
        cpu_is_reading ? 4 : 3;
    // We use tick() since the PPU as as well as the rest of the APU should
    // keep ticking during the fetch
    for (unsigned i = 0; i < delay; ++i) tick();
    dmc_loading_sample_byte = false;
    dmc_sample_buffer_has_data = true;

    dmc_sample_cur_addr = (dmc_sample_cur_addr + 1) & 0x7FFF;
    if (--dmc_bytes_remaining == 0) {
        if (dmc_loop_sample) {
            dmc_sample_cur_addr = dmc_sample_start_addr;
            dmc_bytes_remaining = dmc_sample_len;
        }
        else
            if (dmc_irq_enabled)
                set_dmc_irq(true);
    }
}

static void clock_dmc() {
    if (dpcm_active) {
        if (dmc_shift_reg & 1) {
            if (dmc_counter < 126) {
                dmc_counter += 2;
                channel_updated = true;
            }
        }
        else
            if (dmc_counter > 1) {
                dmc_counter -= 2;
                channel_updated = true;
            }
        dmc_shift_reg >>= 1;
    }

    if (--dmc_bits_remaining == 0) {
        dmc_bits_remaining = 8;

        if ((dpcm_active = dmc_sample_buffer_has_data)) {
            dmc_shift_reg              = dmc_sample_buffer;
            dmc_sample_buffer_has_data = false;
        }
        if (dmc_bytes_remaining > 0)
            load_dmc_sample_byte();
    }
}

//
// Frame counter
//

// Set by the frame counter in 4-step mode, unless inhibited
// Cleared by (derived from Visual 2A03)
//  * the reset signal,
//  * setting the inhibit IRQ flag,
//  * and reading $4015
bool        frame_irq;

static enum Frame_counter_mode { FOUR_STEP = 0, FIVE_STEP = 1 } frame_counter_mode;
static bool inhibit_frame_irq;
static unsigned frame_counter_clock;

static unsigned delayed_frame_timer_reset;

// Quarter frame
static void clock_env_and_tri_lin() {

    // Pulse channels

    for (unsigned n = 0; n < 2; ++n) {
        if (pulse[n].env_start_flag) {
            pulse[n].env_start_flag = false;

            pulse[n].env_vol     = 15;
            pulse[n].env_div_cnt = pulse[n].vol;
        }
        else {
            if (pulse[n].env_div_cnt-- == 0) {
                pulse[n].env_div_cnt = pulse[n].vol;

                if (pulse[n].env_vol > 0)
                    --pulse[n].env_vol;
                else
                    if (pulse[n].halt_len_loop_env)
                        pulse[n].env_vol = 15;
            }
        }
        update_pulse_output_level(n);
    }

    // Noise channel

    if (noise_env_start_flag) {
        noise_env_start_flag = false;

        noise_env_vol     = 15;
        noise_env_div_cnt = noise_vol;
    }
    else {
        if (noise_env_div_cnt-- == 0) {
            noise_env_div_cnt = noise_vol;

            if (noise_env_vol > 0)
                --noise_env_vol;
            else
                if (noise_halt_len_loop_env)
                    noise_env_vol = 15;
        }
    }
    update_noise_output_level();

    // Triangle channel

    if (tri_lin_cnt_reload_flag) {
        tri_lin_cnt_reload_flag = tri_halt_flag;
        tri_lin_cnt = tri_lin_cnt_load;
    }
    else
        if (tri_lin_cnt > 0)
            --tri_lin_cnt;
}

// Half frame
static void clock_len_and_sweep() {
    for (unsigned n = 0; n < 2; ++n) {
        if (!pulse[n].halt_len_loop_env && pulse[n].len_cnt > 0) {
            --pulse[n].len_cnt;
            update_pulse_output_level(n);
        }

        if (pulse[n].sweep_period_cnt == 0) {
            if (pulse[n].sweep_enabled            &&
                pulse[n].period      >= 8         &&
                pulse[n].sweep_shift != 0         &&
                pulse[n].sweep_target_period >= 0 &&
                pulse[n].sweep_target_period <= 0x7FF) {

                pulse[n].period = pulse[n].sweep_target_period;
                update_sweep_target_period(n);
                update_pulse_output_level(n);
            }
        }

        if (pulse[n].sweep_reload_flag || pulse[n].sweep_period_cnt == 0) {
            pulse[n].sweep_reload_flag = false;
            pulse[n].sweep_period_cnt = pulse[n].sweep_period;
        }
        else
            --pulse[n].sweep_period_cnt;
    }

    if (!tri_halt_flag && tri_len_cnt > 0)
        --tri_len_cnt;

    if (!noise_halt_len_loop_env && noise_len_cnt > 0) {
        --noise_len_cnt;
        update_noise_output_level();
    }
}

void write_frame_counter(uint8_t val) {
    frame_counter_mode = (Frame_counter_mode)(val >> 7);
    if ((inhibit_frame_irq = val & 0x40))
        set_frame_irq(false);

    // There is a delay before the frame counter is reset, the length of which
    // varies depending on if the write happens while apu_clk1 is high or low:
    // http://wiki.nesdev.com/w/index.php/APU_Frame_Counter
    delayed_frame_timer_reset = apu_clk1_is_high ? 4 : 3;

    if (frame_counter_mode == FIVE_STEP) {
        clock_env_and_tri_lin();
        clock_len_and_sweep();
    }
}

// The frame IRQ is set during three consecutive CPU ticks at the end of the
// frame period when in four-step mode, so we factor out this helper
static void check_frame_irq() {
    if (!inhibit_frame_irq)
        set_frame_irq(true);
}

// The actual frame counter counts at half the CPU frequency, but the half and
// quarter frame signals are delayed by one CPU cycle, making it easier to
// treat it as counting in CPU cycles.
//
// T1-T5 are the times in CPU ticks for the quarter frame and half frame
// signals, in ascending order. They differ between NTSC and PAL.
template<unsigned T1, unsigned T2, unsigned T3, unsigned T4, unsigned T5>
static void clock_frame_counter_generic() {
    // Possible optimization: Could be sped up with a down-counter instead of a
    // switch

    switch (frame_counter_mode) {
    case FOUR_STEP:
        if (delayed_frame_timer_reset > 0 && --delayed_frame_timer_reset == 0)
            frame_counter_clock = 0;
        else
            if (++frame_counter_clock == T4 + 2) {
                frame_counter_clock = 0;
                check_frame_irq();
            }

        switch (frame_counter_clock) {
        case T1 + 1: case T3 + 1:
            clock_env_and_tri_lin();
            break;

        case T2 + 1:
            clock_len_and_sweep();
            clock_env_and_tri_lin();
            break;

        case T4:
            check_frame_irq();
            break;

        case T4 + 1:
            check_frame_irq();
            clock_len_and_sweep();
            clock_env_and_tri_lin();
            break;
        }
        break;

    case FIVE_STEP:
        if (delayed_frame_timer_reset > 0 && --delayed_frame_timer_reset == 0)
            frame_counter_clock = 0;
        else
            if (++frame_counter_clock == T5 + 2)
                frame_counter_clock = 0;

        switch (frame_counter_clock) {
        case T2 + 1: case T5 + 1:
            clock_len_and_sweep();
            clock_env_and_tri_lin();
            break;

        case T1 + 1: case T3 + 1:
            clock_env_and_tri_lin();
            break;
        }
        break;

    default: UNREACHABLE
    }
}

// Points to the correct instantiated version for NTSC/PAL
static void (*clock_frame_counter)();

//
// Status
//

uint8_t read_apu_status() {
    uint8_t const res =
      (dmc_irq                   << 7) |
      (frame_irq                 << 6) |
      (cpu_data_bus            & 0x20) | // Open bus
      ((dmc_bytes_remaining > 0) << 4) |
      ((noise_len_cnt       > 0) << 3) |
      ((tri_len_cnt         > 0) << 2) |
      ((pulse[1].len_cnt    > 0) << 1) |
       (pulse[0].len_cnt    > 0);

    set_frame_irq(false);

    return res;
}

void write_apu_status(uint8_t val) {
    for (unsigned n = 0; n < 2; ++n) {
        if (!(pulse[n].enabled = val & (1 << n))) {
            pulse[n].len_cnt = 0;
            update_pulse_output_level(n);
        }
    }

    if (!(tri_enabled = val & 4))
        tri_len_cnt = 0;

    if (!(noise_enabled = val & 8)) {
        noise_len_cnt = 0;
        update_noise_output_level();
    }

    // We need to clear the DMC IRQ before handling the DMC enable/disable in
    // case a one-byte sample is loaded below, which will immediately fire a
    // DMC IRQ
    set_dmc_irq(false);

    // DMC enable bit. We model DMC enabled/disabled through the number of
    // sample bytes that remain (greater than zero => enabled).
    if (!(val & 0x10))
        dmc_bytes_remaining = 0;
    else {
        if (dmc_bytes_remaining == 0) {
            dmc_sample_cur_addr = dmc_sample_start_addr;
            dmc_bytes_remaining = dmc_sample_len;
            if (!dmc_sample_buffer_has_data)
                load_dmc_sample_byte();
        }
    }
}

//
// Mixer
//

// Use float instead of double to save some cache. Shouldn't make any
// difference otherwise.
static float pulse_mixer_table[31];
static float tri_noi_dmc_mixer_table[203];

void init_apu() {
    // http://wiki.nesdev.com/w/index.php/APU_Mixer

    pulse_mixer_table[0] = 0;
    for (unsigned n = 1; n < 31; ++n)
        pulse_mixer_table[n] = 95.52/(8128.0/n + 100.0);

    tri_noi_dmc_mixer_table[0] = 0;
    for (unsigned n = 1; n < 203; ++n)
        tri_noi_dmc_mixer_table[n] = 163.67/(24329.0/n + 100.0);
}

void init_apu_for_rom() {
    // Frame counter timing:
    //   http://wiki.nesdev.com/w/index.php/APU_Frame_Counter
    //   http://forums.nesdev.com/viewtopic.php?t=9011
    //
    // TODO: Docs specify 20780 for the final clock in PAL mode, but 20782
    // makes tests pass (including for the next clock after that). Investigate
    // further.

    if (is_pal) {
        clock_frame_counter =
          clock_frame_counter_generic<2*4156, 2*8313, 2*12469, 2*16626, 2*20782>;

        dmc_periods         = pal_dmc_periods;
        noise_periods       = pal_noise_periods;
    }
    else {
        clock_frame_counter =
          clock_frame_counter_generic<2*3728, 2*7456, 2*11185, 2*14914, 2*18640>;

        dmc_periods         = ntsc_dmc_periods;
        noise_periods       = ntsc_noise_periods;
    }
}

void tick_apu() {
    apu_clk1_is_high = !apu_clk1_is_high;

    clock_frame_counter();

    if (!apu_clk1_is_high)
        //
        // Pulse
        //
        for (unsigned n = 0; n < 2; ++n)
            if (--pulse[n].period_cnt == 0) {
                pulse[n].period_cnt = pulse[n].period + 1;
                clock_pulse_generator(n);
            }

    //
    // Triangle
    //

    if (--tri_period_cnt == 0) {
        tri_period_cnt = tri_period + 1;
        clock_triangle_generator();
    }

    //
    // Noise
    //

    if (--noise_period_cnt == 0) {
        noise_period_cnt = noise_period + 1;
        clock_noise_generator();
    }

    //
    // DMC
    //

    if (--dmc_period_cnt == 0) {
        dmc_period_cnt = dmc_period;
        clock_dmc();
    }

    //
    // Mixing
    //

    if (channel_updated) {
        // Possible optimization: Could use integer math and prebias here
        int const signal_level =
          INT16_MIN +
            (pulse_mixer_table[pulse[0].output_level + pulse[1].output_level] +
             tri_noi_dmc_mixer_table[tri_output_level + noise_output_level +
                                     dmc_counter])*(INT16_MAX - INT16_MIN);
        assert(signal_level <= INT16_MAX);
        set_audio_signal_level(signal_level);

        channel_updated = false;
    }
}

//
// Initialization and resetting
//

void reset_apu() {
    // Things explicitly initialized by the reset signal, derived from tracing
    // the _res node in Visual 2A03

    apu_clk1_is_high = false;
    oam_dma_state    = OAM_DMA_NOT_IN_PROGRESS;

    // Pulse channels

    for (unsigned n = 0; n < 2; ++n) {
        pulse[n].enabled          = false;
        pulse[n].waveform_pos     = 0;
        pulse[n].len_cnt          = 0;
        pulse[n].period_cnt       = 1;
        pulse[n].sweep_period_cnt = 0;
        pulse[n].env_div_cnt      = 0;
        pulse[n].env_vol          = 0;
    }

    // Triangle channel

    tri_enabled      = false;
    tri_period_cnt   = 1;
    tri_waveform_pos = 0;
    tri_len_cnt      = 0;
    tri_lin_cnt      = 0;

    // Noise channel

    noise_enabled     = false;
    noise_period      = noise_period_cnt = noise_periods[0];
    noise_len_cnt     = 0;
    noise_shift_reg   = 1; // Essential for LFSR to work
    noise_env_vol     = 0;
    noise_env_div_cnt = 0;

    // DMC channel

    dmc_period_cnt             = dmc_period = dmc_periods[0];
    dmc_sample_cur_addr        = 0x4000;
    dmc_bytes_remaining        = 0;
    dmc_sample_buffer_has_data = false;
    dmc_bits_remaining         = 8;
    // The value here shouldn't matter, but this seems to be what the reset
    // signal does
    dmc_shift_reg              = 0xFF;
    dpcm_active                = false;

    // Frame counter

    delayed_frame_timer_reset = frame_counter_clock = 0;

    // IRQ sources

    set_dmc_irq(false);
    set_frame_irq(false);

    if (frame_counter_mode == FIVE_STEP) {
        clock_env_and_tri_lin();
        clock_len_and_sweep();
    }

    // Set the initial output levels

    for (unsigned n = 0; n < 2; ++n) {
        update_sweep_target_period(n);
        update_pulse_output_level(n);
    }

    update_noise_output_level();

    // Avoids a pop due to a sudden volume change when the triangle starts
    // playing
    tri_output_level = tri_waveform_steps[tri_waveform_pos];
}

void set_apu_cold_boot_state() {
    // Things that do not get initialized by the reset signal are initialized
    // here. They're mostly guesses, but some values being off probably isn't
    // hugely important.

    // Pulse channels

    for (unsigned n = 0; n < 2; ++n) {
        pulse[n].const_vol         = false;
        pulse[n].duty              = 0;
        pulse[n].period            = 0;
        pulse[n].sweep_enabled     = false;
        pulse[n].sweep_negate      = false;
        pulse[n].sweep_period      = 0;
        pulse[n].sweep_shift       = 0;
        pulse[n].sweep_reload_flag = false;
        pulse[n].vol               = 0;

        pulse[n].halt_len_loop_env = false;
        pulse[n].env_start_flag    = false;
    }

    // Triangle channel

    tri_period              = 0;
    tri_halt_flag           = false;
    tri_lin_cnt_load        = 0;
    tri_lin_cnt_reload_flag = false;

    // Noise channel

    noise_halt_len_loop_env = false;
    noise_const_vol         = false;
    noise_vol               = 0;
    noise_feedback_bit      = 1; // Noise looping off
    noise_env_start_flag    = false;

    // DMC channel

    dmc_counter             = 0;
    dmc_irq_enabled         = false;
    dmc_loop_sample         = false;
    dmc_sample_start_addr   = 0x4000;
    dmc_sample_len          = 1;
    dmc_sample_buffer       = 0;
    dmc_loading_sample_byte = false;

    // Frame counter

    frame_counter_mode = FOUR_STEP;
    inhibit_frame_irq  = false;

    // Reset signal takes care of the rest
    reset_apu();
}

//
// State transfers
//

template<bool calculating_size, bool is_save>
void transfer_apu_state(uint8_t *&buf) {
    TRANSFER(apu_clk1_is_high)
    TRANSFER(oam_dma_state)

    // Pulse channel

    for (unsigned i = 0; i < 2; ++i) {
        TRANSFER(pulse[i].output_level)
        TRANSFER(pulse[i].enabled)
        TRANSFER(pulse[i].const_vol)
        TRANSFER(pulse[i].duty)
        TRANSFER(pulse[i].waveform_pos);
        TRANSFER(pulse[i].len_cnt)
        TRANSFER(pulse[i].period)
        TRANSFER(pulse[i].period_cnt)
        TRANSFER(pulse[i].sweep_target_period)
        TRANSFER(pulse[i].sweep_enabled)
        TRANSFER(pulse[i].sweep_negate)
        TRANSFER(pulse[i].sweep_period)
        TRANSFER(pulse[i].sweep_period_cnt)
        TRANSFER(pulse[i].sweep_shift)
        TRANSFER(pulse[i].sweep_reload_flag)
        TRANSFER(pulse[i].vol)

        TRANSFER(pulse[i].env_div_cnt)
        TRANSFER(pulse[i].env_vol)
        TRANSFER(pulse[i].halt_len_loop_env)
        TRANSFER(pulse[i].env_start_flag)
    }

    // Triangle channel

    TRANSFER(tri_output_level)
    TRANSFER(tri_enabled)
    TRANSFER(tri_period)
    TRANSFER(tri_period_cnt)
    TRANSFER(tri_waveform_pos)
    TRANSFER(tri_len_cnt)
    TRANSFER(tri_halt_flag)
    TRANSFER(tri_lin_cnt_load)
    TRANSFER(tri_lin_cnt)
    TRANSFER(tri_lin_cnt_reload_flag)

    // Noise channel

    TRANSFER(noise_output_level)
    TRANSFER(noise_enabled)
    TRANSFER(noise_halt_len_loop_env)
    TRANSFER(noise_const_vol)
    TRANSFER(noise_vol)
    TRANSFER(noise_feedback_bit)
    TRANSFER(noise_period)
    TRANSFER(noise_period_cnt)
    TRANSFER(noise_len_cnt)
    TRANSFER(noise_shift_reg)
    TRANSFER(noise_env_start_flag)
    TRANSFER(noise_env_vol)
    TRANSFER(noise_env_div_cnt)

    update_noise_output_level();

    // DMC channel

    TRANSFER(dmc_counter)
    TRANSFER(dmc_irq_enabled)
    TRANSFER(dmc_loop_sample)
    TRANSFER(dmc_period)
    TRANSFER(dmc_period_cnt)
    TRANSFER(dmc_sample_start_addr)
    TRANSFER(dmc_sample_len)
    TRANSFER(dmc_sample_buffer)
    TRANSFER(dmc_sample_buffer_has_data)
    TRANSFER(dmc_shift_reg)
    TRANSFER(dpcm_active)
    TRANSFER(dmc_loading_sample_byte)
    TRANSFER(dmc_sample_cur_addr)
    TRANSFER(dmc_bytes_remaining)
    TRANSFER(dmc_bits_remaining)

    // Frame counter

    TRANSFER(frame_counter_mode)
    TRANSFER(inhibit_frame_irq)
    TRANSFER(frame_counter_clock)
    TRANSFER(delayed_frame_timer_reset)
}

// Explicit instantiations

// Calculating state size
template void transfer_apu_state<true, false>(uint8_t*&);
// Saving state to buffer
template void transfer_apu_state<false, true>(uint8_t*&);
// Loading state from buffer
template void transfer_apu_state<false, false>(uint8_t*&);
