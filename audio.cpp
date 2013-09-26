#include "common.h"

#include "apu.h"
#include "audio.h"
#include "blip_buf.h"
#include "timing.h"
#include "sdl_backend.h"

// To avoid an immediate underflow, we wait for the audio buffer to fill up
// before we start playing. This is set true when we're happy with the fill
// level.
static bool playback_started;

// blip_buf configuration
unsigned const audio_frames_per_second = 50;
unsigned const audio_frame_len = ntsc_cpu_clock_rate/audio_frames_per_second;
static unsigned audio_frame_offset;

static blip_t *blip;

void init_audio() {
    // Maximum number of unread samples the buffer can hold
    blip = blip_new(sample_rate/10);
    blip_set_rates(blip, ntsc_cpu_clock_rate, sample_rate);
}

void deinit_audio() {
    blip_delete(blip);
}

// Leave 0.3x worth of extra room in the sample buffer to allow audio to be
// sped up by at least that amount
static int16_t blip_samples[(unsigned)(1.3*sample_rate/audio_frames_per_second)];

static void generate_samples() {
    blip_end_frame(blip, audio_frame_len);

    if (playback_started) {
        // Fudge playback rate by an amount proportional to the difference
        // between the desired and current buffer fill levels to try to steer
        // towards it

        static unsigned adjust_fudge_counter = 0;
        if (++adjust_fudge_counter == 8) {
            double const fudge_factor = 1.0 + 0.03*(0.5 - audio_buf_fill_level());

            blip_set_rates(blip, ntsc_cpu_clock_rate, sample_rate*fudge_factor);
            adjust_fudge_counter = 0;
        }
    }
    else {
        if (audio_buf_fill_level() >= 0.5) {
            start_audio_playback();
            playback_started = true;
        }
    }

    int const n_samples = blip_read_samples(blip, blip_samples, ARRAY_LEN(blip_samples), 0);
    // We expect to read all samples from blip_buf. If something goes wrong and
    // we don't, clear the buffer to prevent data piling up in blip_buf's
    // buffer (which lacks bounds checking).
    if (blip_samples_avail(blip) != 0) {
        puts("Warning: didn't read all samples from blip_buf - dropping samples");
        blip_clear(blip);
    }
    add_audio_samples(blip_samples, n_samples);
}

void set_audio_signal_level(int16_t level) {
    // TODO: Do something to reduce the initial pop here?
    static int16_t previous_signal_level;
    blip_add_delta(blip, audio_frame_offset, level - previous_signal_level);
    previous_signal_level = level;
}

void tick_audio() {
    if (++audio_frame_offset == audio_frame_len) {
        generate_samples();
        audio_frame_offset = 0;
    }
}
