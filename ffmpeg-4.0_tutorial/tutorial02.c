// tutorial02.c
// Code based on a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
// Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1
// With updates from https://github.com/chelyaev/ffmpeg-tutorial
// Updates tested on:
// LAVC 54.59.100, LAVF 54.29.104, LSWS 2.1.101
// on GCC 4.7.2 in Debian February 2015

// A small sample program that shows how to use libavformat and libavcodec to
// read video from a file.
//
// Use
//
//gcc ./tutorial01.c  -o ./tutorial01 -lavutil -lavformat -lavcodec -lswscale -lz -lm -I /usr/local/include -L /usr/local/lib/
// gcc -o tutorial01 tutorial01.c -lavformat -lavcodec -lswscale -lz
//
// to build (assuming libavformat and libavcodec are correctly installed
// your system).
//
// Run using
//
// tutorial01 myvideofile.mpg
//
// to write the first five frames from "myvideofile.mpg" to disk in PPM
// format.

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#ifdef __MINGW32__
#undef main /* Prevents SDL from overriding main() */
#endif

#include <stdio.h>

static const struct sdl_texture_format_entry {
    enum AVPixelFormat format; int texture_fmt;
} sdl_texture_format_map[] = {

    { AV_PIX_FMT_RGB8,    SDL_PIXELFORMAT_RGB332 },
    { AV_PIX_FMT_RGB444,  SDL_PIXELFORMAT_RGB444 },
    { AV_PIX_FMT_RGB555,  SDL_PIXELFORMAT_RGB555 },
    { AV_PIX_FMT_BGR555,  SDL_PIXELFORMAT_BGR555 },
    { AV_PIX_FMT_RGB565,  SDL_PIXELFORMAT_RGB565 },
    { AV_PIX_FMT_BGR565,  SDL_PIXELFORMAT_BGR565 },
    { AV_PIX_FMT_RGB24,   SDL_PIXELFORMAT_RGB24 },
    { AV_PIX_FMT_BGR24,   SDL_PIXELFORMAT_BGR24 },
    { AV_PIX_FMT_0RGB32,  SDL_PIXELFORMAT_RGB888 },
    { AV_PIX_FMT_0BGR32,  SDL_PIXELFORMAT_BGR888 },
#if HAVE_BIGENDIAN
    { AV_PIX_FMT_RGB0,    SDL_PIXELFORMAT_RGBX8888 },
    { AV_PIX_FMT_BGR0,    SDL_PIXELFORMAT_BGRX8888 },
#else
    { AV_PIX_FMT_0BGR,    SDL_PIXELFORMAT_RGBX8888 },
    { AV_PIX_FMT_0RGB,    SDL_PIXELFORMAT_BGRX8888 },
#endif
    { AV_PIX_FMT_RGB32,   SDL_PIXELFORMAT_ARGB8888 },
    { AV_PIX_FMT_RGB32_1, SDL_PIXELFORMAT_RGBA8888 },
    { AV_PIX_FMT_BGR32,   SDL_PIXELFORMAT_ABGR8888 },
    { AV_PIX_FMT_BGR32_1, SDL_PIXELFORMAT_BGRA8888 },
    { AV_PIX_FMT_YUV420P, SDL_PIXELFORMAT_IYUV },
    { AV_PIX_FMT_YUYV422, SDL_PIXELFORMAT_YUY2 },
    { AV_PIX_FMT_UYVY422, SDL_PIXELFORMAT_UYVY },
    { AV_PIX_FMT_NONE,    0                },
};

int randomInt(int min, int max)
{
    return min + rand() % (max - min + 1);
}

struct SwsContext *sws_ctx = NULL;
 AVFrame     * pFrameYUV = NULL;
//SDL_CreateTexture();
SDL_Texture    *bmp = NULL;
SDL_Renderer *renderer=NULL;
SDL_Rect        rect;
static const char *src_filename = NULL;
/* Enable or disable frame reference counting. You are not supposed to support
 * both paths in your application but pick the one most appropriate to your
 * needs. Look for the use of refcount in this example to see what are the
 * differences of API usage between them. */
static int refcount = 0;

static void decode(AVCodecContext *dec_ctx, AVPacket *pkt, AVFrame *frame,
                   FILE *outfile)
{
    int ret;

    /* send the packet with the compressed data to the decoder */
    ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error submitting the packet to the decoder\n");
        exit(1);
    }

    /* read all the output frames (in general there may be any number of them */
    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            fprintf(stderr, "Error during decoding\n");
            exit(1);
        }

        // Did we get a video frame?
        sws_scale
        (
             sws_ctx,
             (uint8_t const * const *)frame->data,
             frame->linesize,
             0,
             dec_ctx->height,
             pFrameYUV->data,
             pFrameYUV->linesize
         );
        ////iPitch ����yuvһ������ռ���ֽ���
        SDL_UpdateTexture( bmp, &rect, pFrameYUV->data[0], pFrameYUV->linesize[0] );
        SDL_RenderClear( renderer );
        SDL_RenderCopy( renderer, bmp, &rect, &rect );
        SDL_RenderPresent( renderer );
    }
}

static int open_codec_context(int *stream_idx,
                              AVCodecContext **dec_ctx, AVFormatContext *fmt_ctx, enum AVMediaType type)
{
    int ret, stream_index;
    AVStream *st;
    AVCodec *dec = NULL;
    AVDictionary *opts = NULL;

    ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not find %s stream in input file '%s'\n",
                av_get_media_type_string(type), src_filename);
        return ret;
    } else {
        stream_index = ret;
        st = fmt_ctx->streams[stream_index];

        /* find decoder for the stream */
        dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) {
            fprintf(stderr, "Failed to find %s codec\n",
                    av_get_media_type_string(type));
            return AVERROR(EINVAL);
        }

        /* Allocate a codec context for the decoder */
        *dec_ctx = avcodec_alloc_context3(dec);
        if (!*dec_ctx) {
            fprintf(stderr, "Failed to allocate the %s codec context\n",
                    av_get_media_type_string(type));
            return AVERROR(ENOMEM);
        }

        /* Copy codec parameters from input stream to output codec context */
        if ((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0) {
            fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
                    av_get_media_type_string(type));
            return ret;
        }

        /* Init the decoders, with or without reference counting */
        av_dict_set(&opts, "refcounted_frames", refcount ? "1" : "0", 0);
        if ((ret = avcodec_open2(*dec_ctx, dec, &opts)) < 0) {
            fprintf(stderr, "Failed to open %s codec\n",
                    av_get_media_type_string(type));
            return ret;
        }
        *stream_idx = stream_index;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    AVFormatContext *pFormatCtx = NULL;
    int             i, videoStream;
    AVCodecContext  *pCodecCtx = NULL;
    AVFrame         *pFrame = NULL;
    AVPacket        packet;

    SDL_Window     *screen = NULL;
    SDL_Event       event;

    int sdl_texture_fmt;

    if(argc < 2) {
        fprintf(stderr, "Usage: test <file>\n");
        exit(1);
    }

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }

    src_filename = argv[1];

    // Open video file
    if(avformat_open_input(&pFormatCtx, src_filename, NULL, NULL)!=0)
        return -1; // Couldn't open file

    // Retrieve stream information
    if(avformat_find_stream_info(pFormatCtx, NULL)<0)
        return -1; // Couldn't find stream information

    // Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, argv[1], 0);


    if (open_codec_context(&videoStream, &pCodecCtx, pFormatCtx, AVMEDIA_TYPE_VIDEO) >= 0) {

        /* allocate image where the decoded image will be put */
//        width = video_dec_ctx->width;
//        height = video_dec_ctx->height;
//        pix_fmt = video_dec_ctx->pix_fmt;
//        ret = av_image_alloc(video_dst_data, video_dst_linesize,
//                             width, height, pix_fmt, 1);
//        if (ret < 0) {
//            fprintf(stderr, "Could not allocate raw video buffer\n");
//            goto end;
//        }
//        video_dst_bufsize = ret;
    }

    // Allocate video frame
    pFrame=av_frame_alloc();

    pFrameYUV = av_frame_alloc();
    if( pFrameYUV == NULL )
        return -1;

    // Make a screen to put our video
    screen = SDL_CreateWindow("FFmpeg tutorial01 Window",
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED,
                              pCodecCtx->width,  pCodecCtx->height,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    renderer = SDL_CreateRenderer(screen, -1, 0);


    if(!screen) {
        fprintf(stderr, "SDL: could not set video mode - exiting\n");
        exit(1);
    }

    for (i = 0; sdl_texture_format_map[i].format != AV_PIX_FMT_NONE; i++) {
        if (sdl_texture_format_map[i].format == pCodecCtx->pix_fmt) {
            sdl_texture_fmt = sdl_texture_format_map[i].texture_fmt;
            break;
        }
    }

    // Allocate a place to put our YUV image on that screen
    bmp = SDL_CreateTexture(renderer,sdl_texture_fmt,
                            SDL_TEXTUREACCESS_STREAMING,
                            pCodecCtx->width,pCodecCtx->height);

    sws_ctx =
    sws_getContext
    (
     pCodecCtx->width,
     pCodecCtx->height,
     pCodecCtx->pix_fmt,
     pCodecCtx->width,
     pCodecCtx->height,
     pCodecCtx->pix_fmt,
     SWS_BILINEAR,
     NULL,
     NULL,
     NULL
     );

    // Determine required buffer size and allocate buffer
    int numBytes = av_image_get_buffer_size(pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height, 16);
    uint8_t* buffer=(uint8_t *)av_malloc(numBytes*sizeof(uint8_t));

    // Assign appropriate parts of buffer to image planes in pFrameYUV
    // Note that pFrameYUV is an AVFrame, but AVFrame is a superset
    // of AVPicture
    av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize,
                                buffer, pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height, 1);

    // Read frames and save first five frames to disk
    i=0;

    rect.x = 0;
    rect.y = 0;
    rect.w = pCodecCtx->width;
    rect.h = pCodecCtx->height;

    while(av_read_frame(pFormatCtx, &packet)>=0) {
        // Is this a packet from the video stream?
        if(packet.stream_index==videoStream) {
            // Decode video frame

            decode(pCodecCtx, &packet, pFrame, NULL);

            SDL_Delay(10);
        }

        // Free the packet that was allocated by av_read_frame
        av_packet_unref(&packet);
        av_init_packet(&packet);

        SDL_PollEvent(&event);
        switch(event.type) {
            case SDL_QUIT:
                SDL_Quit();
                exit(0);
                break;
            default:
                break;
        }

    }

    SDL_DestroyTexture(bmp);

    // Free the YUV frame
    av_free(pFrame);
    av_free(pFrameYUV);

    // Close the codecs
    avcodec_free_context(&pCodecCtx);

    // Close the video file
    avformat_close_input(&pFormatCtx);

    return 0;
}
