#include "common.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/avstring.h"
#include "libavutil/avutil.h"
#include "libavutil/mathematics.h"
#include "libswscale/swscale.h"
}

#include <SDL_endian.h>

unsigned const          vid_scale_factor = 3;

char const*const        filename = "movie.avi";

static AVFormatContext *output_ctx;
static AVOutputFormat  *output_fmt;

static AVCodec         *video_encoder;
static AVCodecContext  *video_encoder_ctx;
static AVStream        *video_stream;

static AVFrame         *frame;
// Stores the encoded image for formats that expect us to do the encoding
// rather than accepting a raw AVPicture
static uint8_t         *encoded_frame_buf;
// The old API used here doesn't provide a way to estimate the maximum size of
// the encoded frame, so we'll have to make a guess
size_t const            encoded_frame_buf_size = 1024*400;

// Converts from ARGB to the video's frame format and scales
static SwsContext      *convert_from_argb_ctx;

static void check_av_error(int err, char const *msg) {
    if (err < 0) {
        static char err_msg_buf[128];
        av_strerror(err, err_msg_buf, sizeof(err_msg_buf));
        fail("%s: %s", msg, err_msg_buf);
    }
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

    // Find an encoder for the video format
    fail_if(output_fmt->video_codec == CODEC_ID_NONE,
      "the output format does not appear to support video");
    fail_if(!(video_encoder = avcodec_find_encoder(output_fmt->video_codec)),
      "faled to find a video encoder");

    // Allocate and initialize the video stream

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
    check_av_error(avcodec_open2(video_encoder_ctx, 0, 0), "failed to open encoder");

    // Allocate buffers for frame

    if (!(output_ctx->oformat->flags & AVFMT_RAWPICTURE))
        // The output format needs us to encode the frame
        fail_if(!(encoded_frame_buf = (uint8_t*)av_malloc(encoded_frame_buf_size)),
          "failed to allocate encode buffer for video frame");

    fail_if(!(frame = avcodec_alloc_frame()), "failed to allocate frame structure");
    int const frame_size = avpicture_get_size(video_encoder_ctx->pix_fmt,
      video_encoder_ctx->width, video_encoder_ctx->height);
    uint8_t *const frame_buf = (uint8_t*)av_malloc(frame_size);
    fail_if(!frame_buf, "failed to allocate memory buffer for frame");
    avpicture_fill((AVPicture*)frame, frame_buf,
      video_encoder_ctx->pix_fmt, video_encoder_ctx->width, video_encoder_ctx->height);

    fail_if(!(convert_from_argb_ctx = sws_getCachedContext(0,
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
      "failed to create ARGB-to-video-pixel-format converter");


    // Print format information
    av_dump_format(output_ctx, 0, filename, 1);


    // If the format has an associated file, open it
    if (!(output_fmt->flags & AVFMT_NOFILE))
        check_av_error(avio_open(&output_ctx->pb, filename, AVIO_FLAG_WRITE),
          "failed to open video output file");

    // Write stream header, if any
    check_av_error(avformat_write_header(output_ctx, 0), "failed to write movie header");
}

void add_movie_frame(uint32_t *frame_data) {
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
    sws_scale(convert_from_argb_ctx, (uint8_t const*const*)frame_pic.data, frame_pic.linesize,
      0, 240, frame->data, frame->linesize);

    AVPacket pkt;
    av_init_packet(&pkt);

    int write_res = 0;

    if (output_ctx->oformat->flags & AVFMT_RAWPICTURE) {
        // The format wants an AVPicture with the raw picture data; no need to
        // encode
        pkt.flags        |= AV_PKT_FLAG_KEY; // All frames are key frames
        pkt.stream_index  = video_stream->index;
        pkt.data          = (uint8_t*)frame;
        pkt.size          = sizeof(AVPicture);

        write_res = av_interleaved_write_frame(output_ctx, &pkt);
    }
    else {
        // Encode frame
        int const frame_size = avcodec_encode_video(video_encoder_ctx,
          encoded_frame_buf, encoded_frame_buf_size, frame);
        fail_if(frame_size < 0, "failed to encode video frame");

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
    check_av_error(av_write_trailer(output_ctx),
      "failed to write video trailer");

    // Close the output file, if any
    if (!(output_fmt->flags & AVFMT_NOFILE))
        avio_close(output_ctx->pb);

    sws_freeContext(convert_from_argb_ctx);

    avcodec_close(video_stream->codec);
    av_free(frame->data[0]); // frees frame_buf
    av_free(frame);
    av_free(encoded_frame_buf);

    // Free the output context along with its streams
    avformat_free_context(output_ctx);
}
