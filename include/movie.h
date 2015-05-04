void init_movie();
void end_movie();

void add_movie_audio_frame(int16_t *samples, size_t len);
void add_movie_video_frame(uint32_t *frame_argb);
