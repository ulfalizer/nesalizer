// Movie recording using libav. Works out of the box on Ubuntu 13.10 with
// libav 0.8, which meant using some now deprecated APIs.

void init_movie();
void end_movie();

void add_movie_audio_frame(int16_t *samples, size_t len);
void add_movie_video_frame(uint32_t *frame_argb);
