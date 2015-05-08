// Audio buffering, resampling, etc. Makes use of Blargg's blip_buf library.

void init_audio_for_rom();
void deinit_audio_for_rom();

// Sets the instantaneous signal level
void set_audio_signal_level(int16_t level);
// Resamples and buffers the audio generated during one (video) frame
void end_audio_frame();
// Moves up to 'len' samples from the audio buffer to 'dst'. In case of
// underflow, moves all remaining samples and zeroes the remainder of 'dst' (as
// required by SDL2).
void read_samples(int16_t *dst, size_t len);
