// Audio buffering, resampling, etc. Makes use of Blargg's blip_buf library.

#include "common.h"

#include "audio.h"
#include "cpu.h"
#include "blip_buf.h"
#include "save_states.h"
#include "sdl_backend.h"
#include "timing.h"

//
// Audio ring buffer
//

// Make room for 1/6th seconds of delay
static int16_t buf[GE_POW_2(sample_rate/6)];
// Indices from start_index up to but not including end_index (modulo wrapping)
// contain samples
static size_t start_index = 0, end_index = 0;
// True if the last operation was a read. Indicates whether
// start_index == end_index means the buffer is full or empty.
static bool prev_op_was_read = true;

// Moves up to 'len' samples from the ring buffer to 'dst'. In case of
// underflow, moves all remaining samples and zeroes the remainder of 'dst' (as
// required by SDL2).
void read_samples(int16_t *dst, size_t len) {
    assert(start_index < ARRAY_LEN(buf));

    // Move samples from the ring buffer to 'dst' by memcpy()ing contiguous
    // segments

    // How many contiguous bytes are available...?

    size_t contig_avail;
    if ((start_index == end_index && !prev_op_was_read) || start_index > end_index)
        contig_avail = ARRAY_LEN(buf) - start_index;
    else
        contig_avail = end_index - start_index;

    prev_op_was_read = true;

    if (contig_avail >= len) {
        // ...as many as we need. Copy it all in one go.
        memcpy(dst, buf + start_index, sizeof(*buf)*len);
        start_index = (start_index + len) % ARRAY_LEN(buf);
    }
    else {
        // ... less than we need. Copy the contiguous segment first.
        memcpy(dst, buf + start_index, sizeof(*buf)*contig_avail);
        len -= contig_avail;
        assert(len > 0);
        // Move past the contiguous segment - possibly to index 0
        start_index = (start_index + contig_avail) % ARRAY_LEN(buf);
        assert(start_index <= end_index);
        // How many contiguous bytes are available now...?
        size_t const avail = end_index - start_index;
        if (avail >= len) {
            // ...as many as we need. Copy the rest.
            memcpy(dst + contig_avail, buf + start_index, sizeof(*buf)*len);
            start_index += len;
            assert(start_index <= end_index);
        }
        else {
            // ...less than we need. Copy as much as we can and zero-fill
            // the rest of the output buffer, as required by SDL2.
            memcpy(dst + contig_avail, buf + start_index, sizeof(*buf)*avail);
            memset(dst + contig_avail + avail, 0, sizeof(*buf)*(len - avail));
            assert(start_index + avail == end_index);
            start_index = end_index;
#ifndef RUN_TESTS
            puts("audio buffer underflow!");
#endif
        }
    }
}

// Writes up to 'len' samples from 'src' to the ring buffer. In case of
// overflow, writes as many samples as possible and returns 'false'.
static void write_samples(int16_t const *src, size_t len) {
    // Copy samples from 'src' to the ring buffer by memcpy()ing contiguous
    // segments

    // How many contiguous bytes are available...?

    size_t contig_avail;
    if (start_index < end_index || (start_index == end_index && prev_op_was_read))
        contig_avail = ARRAY_LEN(buf) - end_index;
    else
        contig_avail = start_index - end_index;

    prev_op_was_read = false;

    if (contig_avail >= len) {
        // ...as many as we need. Copy it all in one go.
        memcpy(buf + end_index, src, sizeof(*buf)*len);
        end_index = (end_index + len) % ARRAY_LEN(buf);
    }
    else {
        // ...less than we need. Fill the contiguous segment first.
        memcpy(buf + end_index, src, sizeof(*buf)*contig_avail);
        len -= contig_avail;
        assert(len > 0);
        // Move past the contiguous segment - possibly to index 0
        end_index = (end_index + contig_avail) % ARRAY_LEN(buf);
        assert(end_index <= start_index);
        // How many contiguous bytes are available now...?
        size_t const avail = start_index - end_index;
        if (avail >= len) {
            // ...as many as we need. Copy the rest.
            memcpy(buf + end_index, src + contig_avail, sizeof(*buf)*len);
            end_index += len;
            assert(end_index <= start_index);
        }
        else {
            // ...less than we need. Copy as much as we can and drop the
            // rest.
            memcpy(buf + end_index, src + contig_avail, sizeof(*buf)*avail);
            assert(end_index + avail == start_index);
            end_index = start_index;
#ifndef RUN_TESTS
            puts("audio buffer overflow!");
#endif
        }
    }
}

// Returns the fill level of the ring buffer as a double in the range 0.0-1.0.
static double fill_level() {
    double const data_len = (end_index - start_index) % ARRAY_LEN(buf);
    return data_len/ARRAY_LEN(buf);
}

//
// Initialization, resampling, and buffer management
//

static blip_t   *blip;

// We try to keep the internal audio buffer 50% full for maximum protection
// against under- and overflow. To maintain that level, we adjust the playback
// rate slightly depending on the current buffer fill level. This sets the
// maximum adjustment allowed (1.5%), though typical adjustments will be much
// smaller.
double const    max_adjust = 0.015;

// To avoid an immediate underflow, we wait for the audio buffer to fill up
// before we start playing. This is set true when we're happy with the fill
// level.
static bool     playback_started;

// Leave some extra room in the buffer to allow audio to be slowed down. Assume
// PAL, which gives a slightly larger buffer than NTSC. (The expression is
// equivalent to 1.3*sample_rate/frames_per_second, but a compile-time constant
// in C++03.)
// TODO: Make dependent on max_adjust.
static int16_t  blip_samples[1300*sample_rate/pal_milliframes_per_second];

void set_audio_signal_level(int16_t level) {
    // TODO: Do something to reduce the initial pop here?
    static int16_t previous_signal_level = 0;

    unsigned time  = frame_offset;
    int      delta = level - previous_signal_level;

    if (is_backwards_frame) {
        // Flip deltas and add them from the end of the frame to reverse audio.
        // Since the exact length of the frame can't be known in advance, the
        // length of each frame is recorded when it is saved to the rewind
        // buffer.
        //
        // This is easiest to visualize by thinking of deltas as fenceposts and
        // the signal level as spans between them. While rewinding, the signal
        // level that's being set should be considered the one to the left of
        // the fencepost.
        //
        // One complication is the boundary between frames while rewinding -
        // there the final sample added to one frame is not followed in time by
        // the first sample of the next frame. To solve this, we bring the
        // signal level down to zero at the end of each frame, and then adjust
        // it to the correct value in the next frame (when rewinding, "to zero"
        // becomes "from zero", and everything still works out). We also call
        // begin_frame() between frames to invalidate the cached signal level
        // in apu.cpp. Together this allows frames to be mixed-and-matched
        // arbitrarily in time.
        //
        // Thanks to Blargg for help on this.
        time  = get_frame_len() - time;
        delta = -delta;
    }
    blip_add_delta(blip, time, delta);

    previous_signal_level = level;
}

void end_audio_frame() {
    if (frame_offset == 0)
        // No audio added; blip_end_frame() dislikes being called with an
        // offset of 0
        return;

    assert(!(is_backwards_frame && frame_offset != get_frame_len()));

    // Bring the signal level at the end of the frame to zero as outlined in
    // set_audio_signal_level()
    set_audio_signal_level(0);

    blip_end_frame(blip, frame_offset);

    if (playback_started) {
        // Fudge playback rate by an amount proportional to the difference
        // between the desired and current buffer fill levels to try to steer
        // towards it

        double const fudge_factor = 1.0 + 2*max_adjust*(0.5 - fill_level());
        blip_set_rates(blip, cpu_clock_rate, sample_rate*fudge_factor);
    }
    else {
        if (fill_level() >= 0.5) {
            start_audio_playback();
            playback_started = true;
        }
    }

    int const n_samples = blip_read_samples(blip, blip_samples, ARRAY_LEN(blip_samples), 0);
    // We expect to read all samples from blip_buf. If something goes wrong and
    // we don't, clear the buffer to prevent data piling up in blip_buf's
    // buffer (which lacks bounds checking).
    int const avail = blip_samples_avail(blip);
    if (avail != 0) {
        printf("Warning: didn't read all samples from blip_buf (%d samples remain) - dropping samples\n",
          avail);
        blip_clear(blip);
    }

#ifdef RECORD_MOVIE
    add_movie_audio_frame(blip_samples, n_samples);
#endif

    // Save the samples to the audio ring buffer

    lock_audio();
    write_samples(blip_samples, n_samples);
    unlock_audio();
}

void init_audio_for_rom() {
    // Maximum number of unread samples the buffer can hold
    blip = blip_new(sample_rate/10);
    blip_set_rates(blip, cpu_clock_rate, sample_rate);
}

void deinit_audio_for_rom() {
    blip_delete(blip);
}
