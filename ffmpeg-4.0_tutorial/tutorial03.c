// tutorial03.c
// A pedagogical video player that will stream through every video frame as fast as it can
// and play audio (out of sync).
//
// Code based on FFplay, Copyright (c) 2003 Fabrice Bellard,
// and a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
// Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1
// With updates from https://github.com/chelyaev/ffmpeg-tutorial
// Updates tested on:
// LAVC 54.59.100, LAVF 54.29.104, LSWS 2.1.101, SDL 1.2.15
// on GCC 4.7.2 in Debian February 2015
//
// Use
//
//gcc -w -o a.out tutorial03.c -lavutil -lavformat -lavcodec -lswscale -lz -lm -lSDL -lpthread
// gcc -o tutorial03 tutorial03.c -lavformat -lavcodec -lswscale -lz -lm `sdl-config --cflags --libs`
// to build (assuming libavformat and libavcodec are correctly installed,
// and assuming you have sdl-config. Please refer to SDL docs for your installation.)
//
// Run using
// tutorial03 myvideofile.mpg
//
// to play the stream on your screen.

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>

#ifdef __MINGW32__
#undef main /* Prevents SDL from overriding main() */
#endif

#include <stdio.h>
#include <assert.h>

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

PacketQueue audioq;
SDL_Overlay     *bmp;
SDL_Rect        rect;
struct SwsContext *sws_ctx = NULL;
int quit = 0;
static const char *src_filename = NULL;
/* Enable or disable frame reference counting. You are not supposed to support
 * both paths in your application but pick the one most appropriate to your
 * needs. Look for the use of refcount in this example to see what are the
 * differences of API usage between them. */
static int refcount = 0;

void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}
int packet_queue_put(PacketQueue *q, AVPacket *pkt) {

    AVPacketList *pkt1;

    pkt1 = av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;

    SDL_LockMutex(q->mutex);

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
    return 0;
}
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for(;;) {

        if(quit) {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf) {

    static AVPacket pkt;

    int data_size = 0;
    static int 	ret = -1;
    AVFrame            *pframe=NULL;

    pframe = av_frame_alloc();

    for (;;) {
        /* read all the output frames (in general there may be any number of them */
        while (ret >= 0) {
            ret = avcodec_receive_frame(aCodecCtx, pframe);
//            fprintf(stderr, "ret of avcodec_receive_frame is %d\n", ret);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {

                if(packet_queue_get(&audioq, &pkt, 1) < 0) {
                    return -1;
                }

                /* send the packet with the compressed data to the decoder */
                ret = avcodec_send_packet(aCodecCtx, &pkt);
                if (ret < 0) {
                    fprintf(stderr, "Error submitting the packet to the decoder\n");
                    exit(1);
                }
            }
            else if (ret < 0) {
                fprintf(stderr, "Error during decoding\n");
                exit(1);
            }
            data_size = av_get_bytes_per_sample(aCodecCtx->sample_fmt);
            if (data_size < 0) {
                /* This should not occur, checking just for paranoia */
                fprintf(stderr, "Failed to calculate data size\n");
                exit(1);
            }

            int in_samples = pframe->nb_samples;
            short *sample_buffer = (short*)malloc(pframe->nb_samples * 2 * 2);
            memset(sample_buffer, 0, pframe->nb_samples * 4);

            int i=0;
            float *inputChannel0 = (float*)(pframe->extended_data[0]);

            // Mono
            if( pframe->channels == 1 ) {
                for( i=0; i<in_samples; i++ ) {
                    float sample = *inputChannel0++;
                    if( sample < -1.0f ) {
                        sample = -1.0f;
                    } else if( sample > 1.0f ) {
                        sample = 1.0f;
                    }

                    sample_buffer[i] = (int16_t)(sample * 32767.0f);
                }
            } else { // Stereo
                float* inputChannel1 = (float*)(pframe->extended_data[1]);
                for( i=0; i<in_samples; i++) {
                    sample_buffer[i*2] = (int16_t)((*inputChannel0++) * 32767.0f);
                    sample_buffer[i*2+1] = (int16_t)((*inputChannel1++) * 32767.0f);
                }
            }
            //                fwrite(sample_buffer, 2, in_samples*2, pcmOutFp);
            memcpy(audio_buf,sample_buffer,in_samples*4);
            free(sample_buffer);

            if (pframe->nb_samples <= 0)
            {
                continue;
            }

            data_size = pframe->nb_samples * 4;

            av_frame_free(&pframe);
            return data_size;
        }

        if(packet_queue_get(&audioq, &pkt, 1) < 0) {
            return -1;
        }

        /* send the packet with the compressed data to the decoder */
        ret = avcodec_send_packet(aCodecCtx, &pkt);
        if (ret < 0) {
            fprintf(stderr, "Error submitting the packet to the decoder\n");
            exit(1);
        }

        if(quit) {
            return -1;
        }
    }
}

void audio_callback(void *userdata, Uint8 *stream, int len) {

    AVCodecContext *aCodecCtx = (AVCodecContext *)userdata;
    int len1, audio_size;

    static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
    static unsigned int audio_buf_size = 0;
    static unsigned int audio_buf_index = 0;

    while(len > 0) {
        if(audio_buf_index >= audio_buf_size) {
            /* We have already sent all our data; get more */
            audio_size = audio_decode_frame(aCodecCtx, audio_buf);
            if(audio_size < 0) {
                /* If error, output silence */
                audio_buf_size = 1024; // arbitrary?
                memset(audio_buf, 0, audio_buf_size);
            } else {
                audio_buf_size = audio_size;
            }
            audio_buf_index = 0;
        }
        len1 = audio_buf_size - audio_buf_index;
        if(len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);
        len -= len1;
        stream += len1;
        audio_buf_index += len1;
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

static void decode_video(AVCodecContext *dec_ctx, AVPacket *pkt, AVFrame *frame )
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
        SDL_LockYUVOverlay(bmp);

//      AVPicture pict;
        AVFrame pict;
        pict.data[0] = bmp->pixels[0];
        pict.data[1] = bmp->pixels[2];
        pict.data[2] = bmp->pixels[1];

        pict.linesize[0] = bmp->pitches[0];
        pict.linesize[1] = bmp->pitches[2];
        pict.linesize[2] = bmp->pitches[1];

        // Convert the image into YUV format that SDL uses
        sws_scale(sws_ctx, (uint8_t const * const *)frame->data,
                  frame->linesize, 0, dec_ctx->height,
                  pict.data, pict.linesize);

        SDL_UnlockYUVOverlay(bmp);

        rect.x = 0;
        rect.y = 0;
        rect.w = dec_ctx->width;
        rect.h = dec_ctx->height;
        SDL_DisplayYUVOverlay(bmp, &rect);
    }
}

int main(int argc, char *argv[]) {
    AVFormatContext *pFormatCtx = NULL;
    int             videoStream, audioStream;

    AVCodecContext  *vCodecCtx = NULL;
    AVFrame         *pFrame = NULL;
    AVPacket        packet;

    AVCodecContext  *aCodecCtx = NULL;

    SDL_Surface     *screen;
    SDL_Event       event;
    SDL_AudioSpec   wanted_spec, spec;

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

    // Find the audio stream
    if (open_codec_context(&audioStream, &aCodecCtx, pFormatCtx, AVMEDIA_TYPE_AUDIO) >= 0) {

        // Set audio settings from codec info
        wanted_spec.freq = aCodecCtx->sample_rate;
        wanted_spec.format = AUDIO_S16SYS;
        wanted_spec.channels = aCodecCtx->channels;
        wanted_spec.silence = 0;
        wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
        wanted_spec.callback = audio_callback;
        wanted_spec.userdata = aCodecCtx;

        if(SDL_OpenAudio(&wanted_spec, &spec) < 0) {
            fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
            return -1;
        }

        packet_queue_init(&audioq);
        SDL_PauseAudio(0);
    }

    // Find the decoder for the video stream
    if (open_codec_context(&videoStream, &vCodecCtx, pFormatCtx, AVMEDIA_TYPE_VIDEO) >= 0) {
        // Make a screen to put our video

    #ifndef __DARWIN__
        screen = SDL_SetVideoMode(vCodecCtx->width, vCodecCtx->height, 0, 0);
    #else
        screen = SDL_SetVideoMode(vCodecCtx->width, vCodecCtx->height, 24, 0);
    #endif
        if(!screen) {
            fprintf(stderr, "SDL: could not set video mode - exiting\n");
            exit(1);
        }
    }

    // Allocate video frame
    pFrame=av_frame_alloc();

    // Allocate a place to put our YUV image on that screen
    bmp = SDL_CreateYUVOverlay(vCodecCtx->width,
                               vCodecCtx->height,
                               SDL_YV12_OVERLAY,
                               screen);

    // initialize SWS context for software scaling
    sws_ctx = sws_getContext(vCodecCtx->width,
                             vCodecCtx->height,
                             vCodecCtx->pix_fmt,
                             vCodecCtx->width,
                             vCodecCtx->height,
                             vCodecCtx->pix_fmt, //AV_PIX_FMT_YUV420P,
                             SWS_BILINEAR,
                             NULL,
                             NULL,
                             NULL
                             );

    // Read frames and save first five frames to disk
    av_init_packet(&packet);
    while(av_read_frame(pFormatCtx, &packet)>=0) {
        // Is this a packet from the video stream?
        if(packet.stream_index==videoStream) {
            // Decode video frame
//            avcodec_decode_video2(vCodecCtx, pFrame, &frameFinished, &packet);
            decode_video(vCodecCtx, &packet, pFrame);
            av_packet_unref(&packet);
            av_init_packet(&packet);

        } else if(packet.stream_index==audioStream) {
            packet_queue_put(&audioq, &packet);
        } else {
            av_packet_unref(&packet);
            av_init_packet(&packet);
        }

        SDL_Delay(10);

        // Free the packet that was allocated by av_read_frame
        SDL_PollEvent(&event);
        switch(event.type) {
        case SDL_QUIT:
            quit = 1;
            SDL_Quit();
            exit(0);
            break;
        default:
            break;
        }

    }

    // Free the YUV frame
    av_frame_free(&pFrame);

    // Close the codecs

    avcodec_free_context(&vCodecCtx);
    avcodec_free_context(&aCodecCtx);

    // Close the video file
    avformat_close_input(&pFormatCtx);

    return 0;
}
