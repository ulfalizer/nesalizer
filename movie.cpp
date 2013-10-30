#include "common.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/avstring.h"
#include "libavutil/avutil.h"
#include "libavutil/fifo.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libswscale/swscale.h"
}
#include "sdl_backend.h"

#include <SDL_endian.h>

unsigned const                 vid_scale_factor = 3;

char const*const               filename = "movie.avi";

static AVFormatContext        *output_ctx;
static AVOutputFormat         *output_fmt;

// Video

static AVCodec                *video_encoder;
static AVCodecContext         *video_encoder_ctx;
static AVStream               *video_stream;

static AVFrame                *video_frame;
// Stores the encoded image for formats that expect us to do the encoding
// rather than accepting a raw AVPicture
static uint8_t                *encoded_frame_buf;
// The old API used here doesn't provide a way to estimate the maximum size of
// the encoded frame, so we'll have to make a guess
size_t const                   encoded_frame_buf_size = 1024*400;

// Scales and converts frames to the video's frame format
static SwsContext             *video_conv_ctx;

// Audio

static AVCodec                *audio_encoder;
static AVCodecContext         *audio_encoder_ctx;
static AVStream               *audio_stream;

static AVFrame                *audio_frame;

// Audio frame size used when the codec can handle variably-sized frames
int const                      audio_frame_var_n_samples = 1024*10;
// Number of samples per channel in an audio frame
static int                     audio_frame_n_samples;
// Number of bytes in one sample times times the number of channels
static int                     sample_bsize;
// Size of audio frame in bytes
static int                     audio_frame_bsize;

// Temporary storage for converted samples
static uint8_t                *audio_tmp_buf;
static int                     audio_tmp_buf_bsize;

// Converts to the video's audio format and sample rate
static ReSampleContext        *resample_ctx;

static AVFifoBuffer           *audio_fifo;

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
        for (AVSampleFormat const*f = c->sample_fmts; *f != -1; ++f) {
            char const*const s_str = av_get_sample_fmt_name(*f);
            printf(" %s\n", s_str ? s_str : "(unrecognized format)");
        }
}

static void print_video_encoder_info(AVCodec *c) {
    printf("==== Movie video codec: %s ====\n", c->long_name);

    fputs("Supported pixel formats:", stdout);
    if (!c->pix_fmts)
        puts(" (Unknown)");
    else
        for (PixelFormat const*p = c->pix_fmts; *p != -1; ++p) {
            char const*const p_str = av_get_pix_fmt_name(*p);
            printf(" %s\n", p_str ? p_str : "(unrecognized format)");
        }
}

static void init_audio() {
    // Find an encoder for the audio format
    fail_if(!(audio_encoder = avcodec_find_encoder(output_fmt->audio_codec)),
      "failed to find an audio encoder");

    print_audio_encoder_info(audio_encoder);

    fail_if(!(audio_stream = avformat_new_stream(output_ctx, audio_encoder)),
      "failed to allocate audio stream");
    audio_encoder_ctx = audio_stream->codec;
    // Parameters
    audio_encoder_ctx->bit_rate       = 192000;
    audio_encoder_ctx->channel_layout = AV_CH_LAYOUT_MONO;
    audio_encoder_ctx->channels       = 1;
    audio_encoder_ctx->sample_fmt     = AV_SAMPLE_FMT_FLT;
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

    // Must be able to hold at least the number of converted samples per audio
    // frame. Samples are added ~60 times per second, to which we add a safety
    // factor. TODO: Specialize once PAL support is added.
    // !!!why does it need to be so large?
    audio_tmp_buf_bsize = 4.0*sample_bsize*(audio_encoder_ctx->sample_rate/60.0);
    audio_tmp_buf = (uint8_t*)av_malloc(audio_tmp_buf_bsize);

    fail_if(!(resample_ctx = av_audio_resample_init(
      audio_encoder_ctx->channels   , 1                ,
      audio_encoder_ctx->sample_rate, sample_rate      ,
      audio_encoder_ctx->sample_fmt , AV_SAMPLE_FMT_S16,
      16, 10, 0, 0.8)),
      "can not resample from %d Hz to %d channels at %d Hz",
      sample_rate, audio_encoder_ctx->sample_rate, audio_encoder_ctx->channels);

    fail_if(!(audio_fifo = av_fifo_alloc(10*audio_frame_bsize)),
      "failed to allocate audio FIFO");

}

static void init_video() {
    // Find an encoder for the video format
    fail_if(!(video_encoder = avcodec_find_encoder(output_fmt->video_codec)),
      "failed to find a video encoder");

    print_video_encoder_info(video_encoder);

    fail_if(!(video_stream = avformat_new_stream(output_ctx, video_encoder)),
      "failed to allocate video stream");
    video_encoder_ctx = video_stream->codec;
    // Parameters
    video_encoder_ctx->bit_rate      = 400000;
    video_encoder_ctx->width         = vid_scale_factor*256;
    video_encoder_ctx->height        = vid_scale_factor*240;
    // Assume NTSC for now and adjust to exactly 60 FPS
    video_encoder_ctx->time_base.den = 60;
    video_encoder_ctx->time_base.num = 1;
    // Seems widely supported. TODO: Detect suitable video format?
    video_encoder_ctx->pix_fmt       = PIX_FMT_YUV420P;

    // TODO: B frames

    // Some formats want stream headers to be separate
    if (output_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        video_encoder_ctx->flags |= CODEC_FLAG_GLOBAL_HEADER;

    // Open the video encoder
    check_av_error(avcodec_open2(video_encoder_ctx, 0, 0), "failed to open video encoder");

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
      SWS_BICUBIC, 0, 0, 0)),
      "failed to create video pixel converter/scaler");
}

void init_movie() {
    // Initialize libavcodec and register all codecs and formats
    av_register_all();

    // Guess container format and codecs from filename
    fail_if(!(output_fmt = av_guess_format(0, filename, 0)),
      "failed to deduce output format for '%s'", filename);

    // Allocate the output context for writing the file
    fail_if(!(output_ctx = avformat_alloc_context()), "failed to allocate output context");
    output_ctx->oformat = output_fmt;
    fail_if(strlen(filename) + 1 > sizeof(output_ctx->filename),
      "movie filename is too long (max %zi characters allowed)", sizeof(output_ctx->filename) - 1);
    strcpy(output_ctx->filename, filename);

    fail_if(output_fmt->video_codec == CODEC_ID_NONE,
      "the output format does not appear to support video");
    fail_if(output_fmt->audio_codec == CODEC_ID_NONE,
      "the output format does not appear to support audio");

    init_video();
    init_audio();

    // Print format information
    puts("==== Movie format used ====");
    av_dump_format(output_ctx, 0, filename, 1);

    // If the format has an associated file, open it
    if (!(output_fmt->flags & AVFMT_NOFILE))
        check_av_error(avio_open(&output_ctx->pb, filename, AVIO_FLAG_WRITE),
          "failed to open video output file");

    // Write stream header, if any
    check_av_error(avformat_write_header(output_ctx, 0), "failed to write movie header");
}

void add_movie_audio_frame(int16_t *samples, size_t len) {
    int const n_samples = audio_resample(resample_ctx, (short*)audio_tmp_buf, (short*)samples, len);
    int const n_written = av_fifo_generic_write(audio_fifo, audio_tmp_buf, sample_bsize*n_samples, 0);
    if (n_written != sample_bsize*n_samples)
        puts("warning: movie audio FIFO full - discarding samples");

    // Read and add as many audio frames as possible
    while (av_fifo_size(audio_fifo) >= audio_frame_bsize) {
        av_fifo_generic_read(audio_fifo, audio_tmp_buf, audio_frame_bsize, 0);

        // audio_frame->nb_samples is initialized in init_audio()
        check_av_error(avcodec_fill_audio_frame(audio_frame,
          audio_encoder_ctx->channels, audio_encoder_ctx->sample_fmt,
          (uint8_t*)audio_tmp_buf, audio_frame_bsize, 1),
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
            continue;

        pkt.stream_index = audio_stream->index;

        // libav takes ownership of the data buffer in 'pkt' here
        check_av_error(av_interleaved_write_frame(output_ctx, &pkt),
          "failed to write audio frame");
    }
}

void add_movie_video_frame(uint32_t *frame_data) {
    // Scale and convert to the video's frame format
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
    sws_scale(video_conv_ctx, (uint8_t const*const*)frame_pic.data, frame_pic.linesize,
      0, 240, video_frame->data, video_frame->linesize);

    AVPacket pkt;
    av_init_packet(&pkt);

    int write_res = 0;

    if (output_ctx->oformat->flags & AVFMT_RAWPICTURE) {
        // The format wants an AVPicture with the raw picture data; no need to
        // encode
        pkt.flags        |= AV_PKT_FLAG_KEY; // All frames are key frames
        pkt.stream_index  = video_stream->index;
        pkt.data          = (uint8_t*)video_frame;
        pkt.size          = sizeof(AVPicture);

        write_res = av_interleaved_write_frame(output_ctx, &pkt);
    }
    else {
        // Encode frame
        int const frame_size = avcodec_encode_video(video_encoder_ctx,
          encoded_frame_buf, encoded_frame_buf_size, video_frame);
        check_av_error(frame_size, "failed to encode video frame");

        // TODO: "it is very strongly..." for pts, dts, duration in
        // the doc for av_interleaved_write_frame

        // If the size is zero the image was buffered internally, and we won't
        // get any data this frame
        if (frame_size > 0) {
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

            write_res = av_interleaved_write_frame(output_ctx, &pkt);
        }
    }

    check_av_error(write_res, "failed to write video frame");
}

void end_movie() {
    // Write stream trailer, if any
    check_av_error(av_write_trailer(output_ctx),
      "failed to write video trailer");

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

    sws_freeContext(video_conv_ctx);

    avcodec_close(video_stream->codec);
    av_free(video_frame->data[0]); // frees frame_buf
    av_free(video_frame);
    av_free(encoded_frame_buf);

    // Free the output context along with its streams
    avformat_free_context(output_ctx);
}
