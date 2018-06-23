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

static void decode(AVCodecContext *dec_ctx, AVPacket *pkt, AVFrame *frame,
                   FILE *outfile)
{
    int ret, data_size;

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
        data_size = av_get_bytes_per_sample(dec_ctx->sample_fmt);
        if (data_size < 0) {
            /* This should not occur, checking just for paranoia */
            fprintf(stderr, "Failed to calculate data size\n");
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

int main(int argc, char *argv[]) {
    AVFormatContext *pFormatCtx = NULL;
    int             i, videoStream;
    AVCodecContext  *pCodecCtx = NULL;
    AVCodec         *pCodec = NULL;
    AVFrame         *pFrame = NULL;

    AVPacket        packet;

    //float           aspect_ratio;
    AVDictionary    *optionsDict = NULL;

    SDL_Window     *screen = NULL;
    SDL_Event       event;

    int ret=0;
    AVCodecParameters *origin_par = NULL;
    int sdl_texture_fmt;

    if(argc < 2) {
        fprintf(stderr, "Usage: test <file>\n");
        exit(1);
    }

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }

    // Open video file
    if(avformat_open_input(&pFormatCtx, argv[1], NULL, NULL)!=0)
        return -1; // Couldn't open file

    // Retrieve stream information
    if(avformat_find_stream_info(pFormatCtx, NULL)<0)
        return -1; // Couldn't find stream information

    // Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, argv[1], 0);

    // Find the first video stream
    videoStream = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (videoStream < 0) {
      av_log(NULL, AV_LOG_ERROR, "Can't find video stream in input file\n");
      return -1;
    }

    if(videoStream==-1)
      return -1; // Didn't find a video stream

    // Get a pointer to the codec context for the video stream
    origin_par=pFormatCtx->streams[videoStream]->codecpar;

    pCodec = avcodec_find_decoder(origin_par->codec_id);
    if (!pCodec) {
        av_log(NULL, AV_LOG_ERROR, "Can't find decoder\n");
        return -1;
    }

    pCodecCtx = avcodec_alloc_context3(pCodec);
    if (!pCodecCtx) {
        av_log(NULL, AV_LOG_ERROR, "Can't allocate decoder context\n");
        return AVERROR(ENOMEM);
    }

    ret = avcodec_parameters_to_context(pCodecCtx, origin_par);
    if (ret < 0)
        return 1;

    // Open codec
    if(avcodec_open2(pCodecCtx, pCodec, &optionsDict)<0)
        return -1; // Could not open codec

    // Allocate video frame
    pFrame=av_frame_alloc();

    pFrameYUV = av_frame_alloc();
    if( pFrameYUV == NULL )
        return -1;

    // Make a screen to put our video
    screen = SDL_CreateWindow("My Game Window",
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
        if (sdl_texture_format_map[i].format == origin_par->format) {
            sdl_texture_fmt = sdl_texture_format_map[i].texture_fmt;
            break;
        }
    }

    // Allocate a place to put our YUV image on that screen
//    sdl_texture_fmt=SDL_PIXELFORMAT_IYUV;
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
     origin_par->format,
     SWS_BILINEAR,
     NULL,
     NULL,
     NULL
     );

    // Determine required buffer size and allocate buffer
    int numBytes = av_image_get_buffer_size(origin_par->format, pCodecCtx->width, pCodecCtx->height, 16);
    uint8_t* buffer=(uint8_t *)av_malloc(numBytes*sizeof(uint8_t));

    // Assign appropriate parts of buffer to image planes in pFrameRGB
    // Note that pFrameRGB is an AVFrame, but AVFrame is a superset
    // of AVPicture
    av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize,
                                buffer, origin_par->format, pCodecCtx->width, pCodecCtx->height, 1);

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
