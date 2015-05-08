#include "common.h"

#include "rom.h"
#include "sdl_backend.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/avutil.h>
#include <libavutil/fifo.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
#include <SDL_endian.h>

unsigned const         vid_scale_factor = 3;

char const *const      filename = "movie.mp4";

static AVFormatContext *output_ctx;
static AVOutputFormat  *output_fmt;

// Audio

static AVCodec         *audio_encoder;
static AVCodecContext  *audio_encoder_ctx;
static AVStream        *audio_stream;

static AVFrame         *audio_frame;

// Audio frame size used when the codec can handle variably-sized frames
int const              audio_frame_var_n_samples = 1024*10;
// Number of samples per channel in an audio frame
static int             audio_frame_n_samples;
// Number of bytes in one sample times times the number of channels
static int             sample_bsize;
// Size of audio frame in bytes
static int             audio_frame_bsize;

// Temporary storage for converted samples
static uint8_t         *audio_tmp_buf;

// Converts to the video's audio format and sample rate
static ReSampleContext *resample_ctx;

static AVFifoBuffer    *audio_fifo;

// Video

static AVCodec         *video_encoder;
static AVCodecContext  *video_encoder_ctx;
static AVStream        *video_stream;

static AVFrame         *video_frame;
// Stores the encoded image for formats that expect us to do the encoding
// rather than accepting a raw AVPicture
static uint8_t         *encoded_frame_buf;
// The old API used here doesn't provide a way to estimate the maximum size of
// the encoded frame, so we'll have to make a guess
size_t const           encoded_frame_buf_size = 1024*400;

// Scales and converts frames to the video's frame format
static SwsContext      *video_conv_ctx;

// Holds x264 options
static AVDictionary    *video_opts;

// The index of the next video frame to be encoded
static int64_t         frame_n;


static void check_av_error(int err, char const *msg) {
    if (err < 0) {
        static char err_msg_buf[128];
        av_strerror(err, err_msg_buf, sizeof(err_msg_buf));
        fail("%s: %s", msg, err_msg_buf);
    }
}

static void print_audio_encoder_info(AVCodec *c) {
    printf("==== Movie audio codec: %s ====\n", c->long_name);

    fputs("Supported sample formats:", stdout);
    if (!c->sample_fmts)
        puts(" (unknown)");
    else
        for (AVSampleFormat const *f = c->sample_fmts; *f != -1; ++f) {
            char const *const s_str = av_get_sample_fmt_name(*f);
            printf(" %s", s_str ? s_str : "(unrecognized format)");
        }
    putchar('\n');
}

static void print_video_encoder_info(AVCodec *c) {
    printf("==== Movie video codec: %s ====\n", c->long_name);

    fputs("Supported pixel formats:", stdout);
    if (!c->pix_fmts)
        puts(" (Unknown)");
    else
        for (PixelFormat const *p = c->pix_fmts; *p != -1; ++p) {
            char const *const p_str = av_get_pix_fmt_name(*p);
            printf(" %s", p_str ? p_str : "(unrecognized format)");
        }
    putchar('\n');
}


static void init_audio() {
    // Find an AAC encoder
    fail_if(!(audio_encoder = avcodec_find_encoder(CODEC_ID_AAC)),
      "failed to find an audio encoder");

    print_audio_encoder_info(audio_encoder);

    fail_if(!(audio_stream = avformat_new_stream(output_ctx, audio_encoder)),
      "failed to allocate audio stream");
    audio_encoder_ctx = audio_stream->codec;
    // Parameters
    audio_encoder_ctx->bit_rate       = 128000;
    audio_encoder_ctx->channel_layout = AV_CH_LAYOUT_MONO;
    audio_encoder_ctx->channels       = 1;
    audio_encoder_ctx->sample_fmt     = AV_SAMPLE_FMT_S16;
    audio_encoder_ctx->sample_rate    = sample_rate;

    // Some formats want stream headers to be separate
    if (output_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        audio_encoder_ctx->flags |= CODEC_FLAG_GLOBAL_HEADER;

    // Open the audio encoder
    check_av_error(avcodec_open2(audio_encoder_ctx, 0, 0), "failed to open audio encoder");

    fail_if(!(audio_frame = avcodec_alloc_frame()), "failed to allocate audio frame structure");

    if (audio_encoder_ctx->codec->capabilities & CODEC_CAP_VARIABLE_FRAME_SIZE)
        // The codec accepts variably-sized audio frames
        audio_frame_n_samples = audio_frame_var_n_samples;
    else
        // The codec expects all audio frames to have the same size
        audio_frame_n_samples = audio_encoder_ctx->frame_size;

    sample_bsize      = av_get_bytes_per_sample(audio_encoder_ctx->sample_fmt) *
                        audio_encoder_ctx->channels;
    audio_frame_bsize = sample_bsize*audio_frame_n_samples;

    audio_frame->nb_samples = audio_frame_n_samples;

    // Initialize audio resampler/converter
    fail_if(!(resample_ctx = av_audio_resample_init(
      audio_encoder_ctx->channels   , 1                ,
      audio_encoder_ctx->sample_rate, sample_rate      ,
      audio_encoder_ctx->sample_fmt , AV_SAMPLE_FMT_S16,
      16, 10, 0, 0.8)),
      "can not resample from %d Hz to %d channels at %d Hz",
      sample_rate, audio_encoder_ctx->sample_rate, audio_encoder_ctx->channels);

    double const fps = is_pal ? 50.0 : 60.0;

    // Size of audio FIFO and temporary buffer
    unsigned const audio_buf_size =
      3*max(audio_frame_bsize, int(sample_bsize*(audio_encoder_ctx->sample_rate/fps)));

    fail_if(!(audio_fifo = av_fifo_alloc(audio_buf_size)), "failed to allocate audio FIFO");
    audio_tmp_buf = (uint8_t*)av_malloc(audio_buf_size);
}

static void init_video() {
    fail_if(!(video_encoder = avcodec_find_encoder_by_name("libx264")),
      "H.264 encoder (x264) not found");

    print_video_encoder_info(video_encoder);

    fail_if(!(video_stream = avformat_new_stream(output_ctx, video_encoder)),
      "failed to allocate video stream");
    video_encoder_ctx = video_stream->codec;
    // Generic parameters
    video_encoder_ctx->width         = vid_scale_factor*256;
    video_encoder_ctx->height        = vid_scale_factor*240;
    video_encoder_ctx->time_base.den = is_pal ? 50 : 60;
    video_encoder_ctx->time_base.num = 1;
    video_encoder_ctx->pix_fmt       = PIX_FMT_YUV444P;

    // Some formats want stream headers to be separate
    if (output_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        video_encoder_ctx->flags |= CODEC_FLAG_GLOBAL_HEADER;

    // x264 options
    av_dict_set(&video_opts, "preset", "slow"     , 0);
    av_dict_set(&video_opts, "tune"  , "animation", 0);
    av_dict_set(&video_opts, "crf"   , "18"       , 0);

    // Open the video encoder
    check_av_error(avcodec_open2(video_encoder_ctx, 0, &video_opts), "failed to open video encoder");

    // Any remaining options in video_opts correspond to parameters that
    // weren't recognized
    AVDictionaryEntry *t;
    if ((t = av_dict_get(video_opts, "", 0, AV_DICT_IGNORE_SUFFIX)))
        printf("warning: unrecognized codec option '%s'\n", t->key);

    // Allocate buffers for frame

    if (!(output_ctx->oformat->flags & AVFMT_RAWPICTURE))
        // The output format needs us to encode the frame
        fail_if(!(encoded_frame_buf = (uint8_t*)av_malloc(encoded_frame_buf_size)),
          "failed to allocate buffer for encoded video frames");

    fail_if(!(video_frame = avcodec_alloc_frame()), "failed to allocate video frame structure");

    int const frame_size = avpicture_get_size(video_encoder_ctx->pix_fmt,
      video_encoder_ctx->width, video_encoder_ctx->height);
    uint8_t *const frame_buf = (uint8_t*)av_malloc(frame_size);
    fail_if(!frame_buf, "failed to allocate memory buffer for frame");

    avpicture_fill((AVPicture*)video_frame, frame_buf,
      video_encoder_ctx->pix_fmt, video_encoder_ctx->width, video_encoder_ctx->height);

    fail_if(!(video_conv_ctx = sws_getCachedContext(0,
      256, 240,
      // SDL takes endianess into account for pixel formats, libav doesn't
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
      PIX_FMT_BGRA,
#else
      PIX_FMT_ARGB,
#endif
      video_encoder_ctx->width, video_encoder_ctx->height,
      video_encoder_ctx->pix_fmt,
      SWS_POINT, 0, 0, 0)),
      "failed to create video pixel converter/scaler");
}

void init_movie() {
    // Initialize libavcodec and register all codecs and formats
    av_register_all();

    // Guess container format
    fail_if(!(output_fmt = av_guess_format(0, filename, 0)),
      "failed to deduce output format for '%s'", filename);

    // Allocate the output context for writing the file
    fail_if(!(output_ctx = avformat_alloc_context()), "failed to allocate output context");
    output_ctx->oformat = output_fmt;
    fail_if(strlen(filename) + 1 > sizeof(output_ctx->filename),
      "movie filename is too long (max %zu characters allowed)", sizeof(output_ctx->filename) - 1);
    strcpy(output_ctx->filename, filename);

    fail_if(output_fmt->video_codec == CODEC_ID_NONE,
      "the output format does not appear to support video");
    fail_if(output_fmt->audio_codec == CODEC_ID_NONE,
      "the output format does not appear to support audio");

    init_video();
    init_audio();

    puts("==== Movie format ====");
    av_dump_format(output_ctx, 0, filename, 1);

    // If the format has an associated file, open it
    if (!(output_fmt->flags & AVFMT_NOFILE))
        check_av_error(avio_open(&output_ctx->pb, filename, AVIO_FLAG_WRITE),
          "failed to open video output file");

    // Write stream header, if any
    check_av_error(avformat_write_header(output_ctx, 0), "failed to write movie header");
}

static void write_audio_frame(int frame_size) {
    // audio_frame->nb_samples is initialized in init_audio()
    check_av_error(avcodec_fill_audio_frame(audio_frame,
      audio_encoder_ctx->channels, audio_encoder_ctx->sample_fmt,
      (uint8_t*)audio_tmp_buf, frame_size, 1),
      "failed to initialize audio frame");

    AVPacket pkt;
    av_init_packet(&pkt);
    // 'data' and 'size' must be zero to signal to avcodec_encode_audio2()
    // to allocate the buffer for us
    pkt.data = 0;
    pkt.size = 0;

    int got_packet;
    check_av_error(avcodec_encode_audio2(audio_encoder_ctx, &pkt, audio_frame, &got_packet),
      "failed to encode audio frame");
    if (!got_packet)
        return;

    pkt.stream_index = audio_stream->index;

    // libav takes ownership of the data buffer in 'pkt' here
    check_av_error(av_interleaved_write_frame(output_ctx, &pkt),
      "failed to write audio frame");
}

static void write_audio_frames() {
    // Read and add as many audio frames as possible
    while (av_fifo_size(audio_fifo) >= audio_frame_bsize) {
        av_fifo_generic_read(audio_fifo, audio_tmp_buf, audio_frame_bsize, 0);
        write_audio_frame(audio_frame_bsize);
    }

    // Compensate for A/V drift by fudging the resampling rate

    // Maximum number of samples to deliberately deviate from the output sample
    // rate by for A/V synchronization purposes
    int    const max_sample_adjust = 400;

    double const audio_pts    = audio_stream->pts.val * av_q2d(audio_stream->time_base);
    double const video_pts    = frame_n               * av_q2d(video_stream->time_base);
    // Number of samples we are off by compared to how many should have been generated by now
    int    const sample_delta = av_clip((video_pts - audio_pts)*audio_encoder_ctx->sample_rate
                                          - av_fifo_size(audio_fifo)/sample_bsize,
                                        -max_sample_adjust, max_sample_adjust);

    if (abs(sample_delta) > 50)
        av_resample_compensate(*(AVResampleContext**)resample_ctx, sample_delta, audio_encoder_ctx->sample_rate);

    /*
    // A/V synchronization debugging

    static unsigned n;
    if (++n % 100 == 0) {
        printf("Audio PTS: %f\n", audio_stream->pts.val*av_q2d(audio_stream->time_base));
        printf("Video PTS: %f\n", frame_n*av_q2d(video_stream->time_base));
        printf("A/V diff: %f\n", frame_n*av_q2d(video_stream->time_base) - audio_stream->pts.val*av_q2d(audio_stream->time_base));
    }
    */
}

void add_movie_audio_frame(int16_t *samples, size_t len) {
    int const n_samples = audio_resample(resample_ctx, (short*)audio_tmp_buf, (short*)samples, len);
    int const n_written = av_fifo_generic_write(audio_fifo, audio_tmp_buf, sample_bsize*n_samples, 0);
    if (n_written != sample_bsize*n_samples)
        puts("warning: movie audio FIFO full - discarding samples");

    write_audio_frames();
}

static void flush_audio() {
    write_audio_frames();

    // Flush any remaining samples that are too few to constitute a complete
    // audio frame
    int const fifo_bytes = av_fifo_size(audio_fifo);
    if (fifo_bytes > 0) {
        av_fifo_generic_read(audio_fifo, audio_tmp_buf, fifo_bytes, 0);

        int frame_bytes;

        if (audio_encoder_ctx->codec->capabilities & CODEC_CAP_SMALL_LAST_FRAME)
            frame_bytes = fifo_bytes;
        else {
            // Pad last frame with silence
            frame_bytes = audio_frame_bsize;
            // A memset() here generates a spurious linker warning, so use a
            // loop instead
            for (int i = 0; i < frame_bytes - fifo_bytes; ++i)
                // This is what avconv.c does
                audio_tmp_buf[fifo_bytes + i] =
                  (audio_encoder_ctx->sample_fmt == AV_SAMPLE_FMT_U8) ? 0x80 : 0x00;
        }

        write_audio_frame(frame_bytes);
    }
}


static void write_video_frame(int frame_size) {
    // If the size is zero the image was buffered internally (e.g. to get
    // lookahead for B frames), and we won't get any data this frame
    if (frame_size > 0) {
        AVPacket pkt;
        av_init_packet(&pkt);

        if (video_encoder_ctx->coded_frame->pts != AV_NOPTS_VALUE)
            // The format uses PTSs (Presentation Timestamps). Rescale them
            // from the encoder's time base to the video stream's.
            pkt.pts = av_rescale_q(video_encoder_ctx->coded_frame->pts,
              video_encoder_ctx->time_base, video_stream->time_base);
        if (video_encoder_ctx->coded_frame->key_frame)
            pkt.flags |= AV_PKT_FLAG_KEY;
        pkt.stream_index = video_stream->index;
        pkt.data         = encoded_frame_buf;
        pkt.size         = frame_size;

        check_av_error(av_interleaved_write_frame(output_ctx, &pkt),
          "failed to write video frame");
    }
}

void add_movie_video_frame(uint32_t *frame_data) {
    // Scale and convert to the video's pixel format
    AVPicture frame_pic;
    avpicture_fill(&frame_pic,
      (uint8_t*)frame_data,
      // SDL takes endianess into account for pixel formats, libav doesn't
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
      PIX_FMT_BGRA,
#else
      PIX_FMT_ARGB,
#endif
      256, 240);
    sws_scale(video_conv_ctx, (uint8_t const *const*)frame_pic.data, frame_pic.linesize,
      0, 240, video_frame->data, video_frame->linesize);

    if (output_ctx->oformat->flags & AVFMT_RAWPICTURE) {
        AVPacket pkt;
        av_init_packet(&pkt);

        // The format wants an AVPicture with the raw picture data; no need to
        // encode
        pkt.flags        |= AV_PKT_FLAG_KEY; // All frames are key frames
        pkt.stream_index  = video_stream->index;
        pkt.data          = (uint8_t*)video_frame;
        pkt.size          = sizeof(AVPicture);

        check_av_error(av_interleaved_write_frame(output_ctx, &pkt),
          "failed to write video frame (raw frame case)");
    }
    else {
        video_frame->pts = frame_n++;

        // Encode frame
        int const frame_size = avcodec_encode_video(video_encoder_ctx,
          encoded_frame_buf, encoded_frame_buf_size, video_frame);
        check_av_error(frame_size, "failed to encode video frame");

        write_video_frame(frame_size);
    }
}

// Flushes any remaining frames (e.g. due to B frames) from the encoder at the
// end of recording
static void flush_video() {
    if (output_ctx->oformat->flags & AVFMT_RAWPICTURE)
        return;

    for (;;) {
        // Passing null for the last argument gets a delayed frame
        int const frame_size = avcodec_encode_video(video_encoder_ctx,
          encoded_frame_buf, encoded_frame_buf_size, 0);
        check_av_error(frame_size, "failed to encode video frame");

        if (frame_size == 0)
            return;

        write_video_frame(frame_size);
    }
}


void end_movie() {
    flush_audio();
    flush_video();

    // Write stream trailer, if any
    check_av_error(av_write_trailer(output_ctx), "failed to write video trailer");

    // Close the output file, if any
    if (!(output_fmt->flags & AVFMT_NOFILE))
        avio_close(output_ctx->pb);

    // Audio

    audio_resample_close(resample_ctx);
    av_fifo_free(audio_fifo);

    avcodec_close(audio_stream->codec);
    av_free(audio_frame);

    av_free(audio_tmp_buf);

    // Video

    av_dict_free(&video_opts);

    sws_freeContext(video_conv_ctx);

    avcodec_close(video_stream->codec);
    av_free(video_frame->data[0]); // frees frame_buf
    av_free(video_frame);
    av_free(encoded_frame_buf);

    // Free the output context along with its streams
    avformat_free_context(output_ctx);
}
