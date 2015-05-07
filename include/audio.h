void init_audio_for_rom();
void deinit_audio_for_rom();

void set_audio_signal_level(int16_t level);
void end_audio_frame();
void read_samples(int16_t *dst, size_t n_samples);
