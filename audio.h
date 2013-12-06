void init_audio_for_rom();
void deinit_audio_for_rom();

void end_audio_frame();
void set_audio_signal_level(int16_t level);
void tick_audio();

extern unsigned audio_frame_len;
