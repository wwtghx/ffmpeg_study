// tutorial04.c
// A pedagogical video player that will stream through every video frame as fast as it can,
// and play audio (out of sync).
//
// Code based on FFplay, Copyright (c) 2003 Fabrice Bellard,
// and a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
// Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1
// With updates from https://github.com/chelyaev/ffmpeg-tutorial
// Updates tested on:
// LAVC 54.59.100, LAVF 54.29.104, LSWS 2.1.101, SDL 1.2.15
// on GCC 4.7.2 in Debian February 2015
// Use
//
//gcc -w -o a.out tutorial04.c -lavutil -lavformat -lavcodec -lswscale -lz -lm -lSDL -lpthread
//
// gcc -o tutorial04 tutorial04.c -lavformat -lavcodec -lswscale -lz -lm `sdl-config --cflags --libs`
// to build (assuming libavformat and libavcodec are correctly installed,
// and assuming you have sdl-config. Please refer to SDL docs for your installation.)
//
// Run using
// tutorial04 myvideofile.mpg
//
// to play the video stream on your screen.

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
#include <math.h>

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define VIDEO_PICTURE_QUEUE_SIZE 1

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;


typedef struct VideoPicture {
    SDL_Overlay *bmp;
    int width, height; /* source height & width */
    int allocated;
} VideoPicture;

typedef struct VideoState {

    AVFormatContext *pFormatCtx;
    int             videoStream, audioStream;
    AVStream        *audio_st;
    AVCodecContext  *audio_ctx;
    PacketQueue     audioq;
    uint8_t         audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
    unsigned int    audio_buf_size;
    unsigned int    audio_buf_index;
    AVFrame         audio_frame;
    AVPacket        audio_pkt;
    uint8_t         *audio_pkt_data;
    int             audio_pkt_size;
    AVStream        *video_st;
    AVCodecContext  *video_ctx;
    PacketQueue     videoq;
    struct SwsContext *sws_ctx;

    VideoPicture    pictq[VIDEO_PICTURE_QUEUE_SIZE];
    int             pictq_size, pictq_rindex, pictq_windex;
    SDL_mutex       *pictq_mutex;
    SDL_cond        *pictq_cond;

    SDL_Thread      *parse_tid;
    SDL_Thread      *video_tid;

    char            filename[1024];
//    char            *filename;
    int             quit;

    //同步相关
    double             audio_clock;
    double             video_clock;
    double             cur_frame_pts;
    double 			pre_frame_pts; 			//前一帧显示时间
    double 			pre_cur_frame_delay; 	//当前帧和前一帧的延时，前面两个相减的结果
    uint32_t			delay;

    //seek
    int             seek_req;
    int             seek_flags;
    int64_t         seek_pos;

} VideoState;

SDL_Surface     *screen;
SDL_mutex       *screen_mutex;

/* Since we only have one decoding thread, the Big Struct
   can be global in case we need it. */
VideoState *global_video_state;
AVPacket flush_pkt;

void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}
int packet_queue_put(PacketQueue *q, AVPacket *pkt) {

    AVPacketList *pkt1;

    if(pkt != &flush_pkt && av_dup_packet(pkt) < 0) {
        return -1;
    }

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

        if(global_video_state->quit) {
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

    av_init_packet(&pkt);
    for (;;) {
        /* read all the output frames (in general there may be any number of them */
        while (ret >= 0) {
            ret = avcodec_receive_frame(aCodecCtx, pframe);
//            fprintf(stderr, "ret of avcodec_receive_frame is %d\n", ret);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {

                if(packet_queue_get(&global_video_state->audioq, &pkt, 1) < 0) {
                    return -1;
                }
                if(pkt.data == flush_pkt.data) {
                    avcodec_flush_buffers(global_video_state->audio_ctx);
                    continue;
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

            //更新audio clock, 以实现音视频同步
            if (pkt.pts != AV_NOPTS_VALUE)
            {
               global_video_state->audio_clock = pkt.pts*av_q2d(global_video_state->audio_st->time_base);

               printf("音频里：ps->audio_clock = %lf\n", global_video_state->audio_clock);
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

        av_packet_unref(&pkt);
        av_init_packet(&pkt);

        if(packet_queue_get(&global_video_state->audioq, &pkt, 1) < 0) {
            return -1;
        }
        if(pkt.data == flush_pkt.data) {
            avcodec_flush_buffers(global_video_state->audio_ctx);
            continue;
        }
        /* send the packet with the compressed data to the decoder */
        ret = avcodec_send_packet(aCodecCtx, &pkt);
        if (ret < 0) {
            fprintf(stderr, "Error submitting the packet to the decoder\n");
            exit(1);
        }

        if(global_video_state->quit) {
            return -1;
        }
    }
}

void audio_callback(void *userdata, Uint8 *stream, int len) {

    VideoState *is = (VideoState *)userdata;
    int len1, audio_size;

    while(len > 0) {
        if(is->audio_buf_index >= is->audio_buf_size) {
            /* We have already sent all our data; get more */
//            audio_size = audio_decode_frame(is, is->audio_buf, sizeof(is->audio_buf));
            audio_size = audio_decode_frame(is->audio_ctx, is->audio_buf);
            if(audio_size < 0) {
                /* If error, output silence */
                is->audio_buf_size = 1024;
                memset(is->audio_buf, 0, is->audio_buf_size);
            } else {
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if(len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
}

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque) {
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    event.user.data1 = opaque;
    SDL_PushEvent(&event);
    return 0; /* 0 means stop timer */
}

/* schedule a video refresh in 'delay' ms */
static void schedule_refresh(VideoState *is, int delay) {
    SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

void video_display(VideoState *is) {

    SDL_Rect rect;
    VideoPicture *vp;
    float aspect_ratio;
    int w, h, x, y;


    vp = &is->pictq[is->pictq_rindex];
    if(vp->bmp) {
        if(is->video_ctx->sample_aspect_ratio.num == 0) {
            aspect_ratio = 0;
        } else {
            aspect_ratio = av_q2d(is->video_ctx->sample_aspect_ratio) *
                    is->video_ctx->width / is->video_ctx->height;
        }
        if(aspect_ratio <= 0.0) {
            aspect_ratio = (float)is->video_ctx->width /
                    (float)is->video_ctx->height;
        }
        h = screen->h;
        w = ((int)rint(h * aspect_ratio)) & -3;
        if(w > screen->w) {
            w = screen->w;
            h = ((int)rint(w / aspect_ratio)) & -3;
        }
        x = (screen->w - w) / 2;
        y = (screen->h - h) / 2;

        rect.x = x;
        rect.y = y;
        rect.w = w;
        rect.h = h;
        SDL_LockMutex(screen_mutex);
        SDL_DisplayYUVOverlay(vp->bmp, &rect);
        SDL_UnlockMutex(screen_mutex);

    }
}

void video_refresh_timer(void *userdata) {

    VideoState *is = (VideoState *)userdata;
    VideoPicture *vp;

    if(is->video_st) {
        if(is->pictq_size == 0) {
            schedule_refresh(is, 1);
        } else {
            vp = &is->pictq[is->pictq_rindex];
            /* Now, normally here goes a ton of code
     about timing, etc. we're just going to
     guess at a delay for now. You can
     increase and decrease this value and hard code
     the timing - but I don't suggest that ;)
     We'll learn how to do it for real later.
      */
            schedule_refresh(is, is->delay);

            /* show the picture! */
            video_display(is);

            /* update queue for next picture! */
            if(++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
                is->pictq_rindex = 0;
            }
            SDL_LockMutex(is->pictq_mutex);
            is->pictq_size--;
            SDL_CondSignal(is->pictq_cond);
            SDL_UnlockMutex(is->pictq_mutex);
        }
    } else {
        schedule_refresh(is, 10);
    }
}

void alloc_picture(void *userdata) {

    VideoState *is = (VideoState *)userdata;
    VideoPicture *vp;

    vp = &is->pictq[is->pictq_windex];
    if(vp->bmp) {
        // we already have one make another, bigger/smaller
        SDL_FreeYUVOverlay(vp->bmp);
    }
    // Allocate a place to put our YUV image on that screen
    SDL_LockMutex(screen_mutex);
    vp->bmp = SDL_CreateYUVOverlay(is->video_ctx->width,
                                   is->video_ctx->height,
                                   SDL_YV12_OVERLAY,
                                   screen);
    SDL_UnlockMutex(screen_mutex);

    vp->width = is->video_ctx->width;
    vp->height = is->video_ctx->height;
    vp->allocated = 1;

}

int queue_picture(VideoState *is, AVFrame *pFrame) {

    VideoPicture *vp;
    int dst_pix_fmt;
    AVFrame pict;

    /* wait until we have space for a new pic */
    SDL_LockMutex(is->pictq_mutex);
    while(is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE &&
          !is->quit) {
        SDL_CondWait(is->pictq_cond, is->pictq_mutex);
    }
    SDL_UnlockMutex(is->pictq_mutex);

    if(is->quit)
        return -1;

    // windex is set to 0 initially
    vp = &is->pictq[is->pictq_windex];

    /* allocate or resize the buffer! */
    if(!vp->bmp ||
            vp->width != is->video_ctx->width ||
            vp->height != is->video_ctx->height) {

        vp->allocated = 0;
        alloc_picture(is);
        if(is->quit) {
            return -1;
        }
    }

    /* We have a place to put our picture on the queue */

    if(vp->bmp) {

        SDL_LockYUVOverlay(vp->bmp);

        dst_pix_fmt = AV_PIX_FMT_YUV420P;
        /* point pict at the queue */

        pict.data[0] = vp->bmp->pixels[0];
        pict.data[1] = vp->bmp->pixels[2];
        pict.data[2] = vp->bmp->pixels[1];

        pict.linesize[0] = vp->bmp->pitches[0];
        pict.linesize[1] = vp->bmp->pitches[2];
        pict.linesize[2] = vp->bmp->pitches[1];

        // Convert the image into YUV format that SDL uses
        sws_scale(is->sws_ctx, (uint8_t const * const *)pFrame->data,
                  pFrame->linesize, 0, is->video_ctx->height,
                  pict.data, pict.linesize);

        SDL_UnlockYUVOverlay(vp->bmp);
        /* now we inform our display thread that we have a pic ready */
        if(++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
            is->pictq_windex = 0;
        }
        SDL_LockMutex(is->pictq_mutex);
        is->pictq_size++;
        SDL_UnlockMutex(is->pictq_mutex);
    }
    return 0;
}

double get_audio_clock(VideoState *ps);

/*======================================================================\
* Author     (作者): i.sshe
* Date       (日期): 2016/10/08
* Others     (其他): 获取延迟时间
\*=======================================================================*/
double get_delay(VideoState *ps)
{
    double 		ret_delay = 0.0;
    double 		frame_delay = 0.0;
    double 		cur_audio_clock = 0.0;
    double 		compare = 0.0;
    double  	threshold = 0.0;

    //这里的delay是秒为单位， 化为毫秒：*1000
    frame_delay = ps->cur_frame_pts - ps->pre_frame_pts;
    if (frame_delay <= 0 || frame_delay >= 1.0)
    {
        frame_delay = ps->pre_cur_frame_delay;
    }
    //两帧之间的延时
    ps->pre_cur_frame_delay = frame_delay;
    ps->pre_frame_pts = ps->cur_frame_pts;

    cur_audio_clock = get_audio_clock(ps);

    //compare < 0 说明慢了， > 0说明快了
    compare = ps->cur_frame_pts - cur_audio_clock;

    //设置一个阀值, 是一个正数
    //这里设阀值为两帧之间的延迟，
    threshold = frame_delay;
    //SYNC_THRESHOLD ? frame_delay : SYNC_THRESHOLD;


    if (compare <= -threshold) 		//慢， 加快速度
    {
        ret_delay = frame_delay / 2;
    }
    else if (compare >= threshold) 	//快了，就在上一帧延时的基础上加长延时
    {
        ret_delay = frame_delay * 2;
    }
    else
    {
        //
        ret_delay = frame_delay;//frame_delay;
    }

    return ret_delay;
}

static void packet_queue_flush(PacketQueue *q) {
    AVPacketList *pkt, *pkt1;

    SDL_LockMutex(q->mutex);
    for(pkt = q->first_pkt; pkt != NULL; pkt = pkt1) {
        pkt1 = pkt->next;
        av_free_packet(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    SDL_UnlockMutex(q->mutex);
}

/*======================================================================\
* Author     (作者): i.sshe
* Date       (日期): 2016/10/08
* Others     (其他): 获取音频的当前时间
\*=======================================================================*/
double get_audio_clock(VideoState *ps)
{
    long long bytes_per_sec = 0;
    double cur_audio_clock = 0.0;
    double cur_buf_pos = ps->audio_buf_index;

    //每个样本占2bytes。16bit
    bytes_per_sec = ps->audio_st->codecpar->sample_rate
                * ps->audio_ctx->channels * 2;

    cur_audio_clock = ps->audio_clock +
                    cur_buf_pos / (double)bytes_per_sec;
/*
    printf("cur_buf_pos = %lf, bytes_per_sec = %lld, "
            "cur_audio_clock = %lf, audio_clock = %lf\n",
            cur_buf_pos, bytes_per_sec, cur_audio_clock, ps->audio_clock);
*/
    return cur_audio_clock;
}


/*======================================================================\
* Author     (作者): i.sshe
* Date       (日期): 2016/10/08
* Others     (其他): 获取一个帧的pts
\*=======================================================================*/
double get_frame_pts(VideoState *ps, AVFrame *pframe)
{
    double pts = 0.0;
    double frame_delay = 0.0;

    pts = pframe->pts;
    if (pts == AV_NOPTS_VALUE) 		//???
    {
        pts = 0;
    }

    pts *= av_q2d(ps->video_st->time_base);

    if (pts != 0)
    {
        ps->video_clock = pts; 		//video_clock貌似没有什么实际用处
    }
    else
    {
        pts = ps->video_clock;
    }

    //更新video_clock, 这里不理解
    //这里用的是AVCodecContext的time_base
    //extra_delay = repeat_pict / (2*fps), 这个公式是在ffmpeg官网手册看的
    frame_delay = av_q2d(ps->video_st->time_base);

    frame_delay += pframe->repeat_pict / (frame_delay * 2);
    ps->video_clock += frame_delay;

    return pts;
}

static void decode_video(AVCodecContext *dec_ctx, AVPacket *pkt, AVFrame *frame )
{
    int ret;
    VideoState      *ps=global_video_state;
    double pts = 0.0;

    if(pkt->data == flush_pkt.data) {
        avcodec_flush_buffers(global_video_state->video_ctx);
    } else {
        /* send the packet with the compressed data to the decoder */
        ret = avcodec_send_packet(dec_ctx, pkt);
        if (ret < 0) {
            fprintf(stderr, "Error submitting the packet to the decoder\n");
            exit(1);
        }
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

        //下面三句实现音视频同步，还有一句在audio部分。
        //获取pts
        pts = get_frame_pts(ps, frame);

        //ps中用cur_frame_pts是为了减少get_delay()的参数
        ps->cur_frame_pts = pts; //*(double *)pframe.opaque;
        ps->delay = get_delay(ps) * 1000 + 0.5;

        printf("video frame pts = %lf\n", pts);
        printf("显示里面：ps->delay = %d\n", ps->delay);

        if(queue_picture(global_video_state, frame) < 0) {
            break;
        }
    }
}

int video_thread(void *arg) {
    VideoState *is = (VideoState *)arg;
    AVPacket pkt1, *packet = &pkt1;
    AVFrame *pFrame;

    pFrame = av_frame_alloc();

    av_init_packet(packet);
    for(;;) {
        if(packet_queue_get(&is->videoq, packet, 1) < 0) {
            // means we quit getting packets
            break;
        }
        // Decode video frame
//        avcodec_decode_video2(is->video_ctx, pFrame, &frameFinished, packet);
        decode_video(is->video_ctx, packet, pFrame);
        av_packet_unref(packet);
        av_init_packet(packet);
    }
    av_frame_free(&pFrame);
    return 0;
}

int stream_component_open(VideoState *is, int stream_index) {

    AVFormatContext *pFormatCtx = is->pFormatCtx;
    AVCodecContext *codecCtx = NULL;
    AVCodec *codec = NULL;
    SDL_AudioSpec wanted_spec, spec;
    int ret;

    if(stream_index < 0 || stream_index >= pFormatCtx->nb_streams) {
        return -1;
    }

#if 0
    codec = avcodec_find_decoder(pFormatCtx->streams[stream_index]->codecpar->codec_id);
    if(!codec) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }

    codecCtx = avcodec_alloc_context3(codec);

//    if(avcodec_copy_context(codecCtx, pFormatCtx->streams[stream_index]->codec) != 0) {
//        fprintf(stderr, "Couldn't copy codec context");
//        return -1; // Error copying codec context
//    }

    ret = avcodec_parameters_to_context(codecCtx, pFormatCtx->streams[stream_index]->codecpar);
    if (ret < 0)
        return -1;

#else

    codecCtx = avcodec_alloc_context3(NULL);
    if (!codecCtx)
        return AVERROR(ENOMEM);

    codec = avcodec_find_decoder(pFormatCtx->streams[stream_index]->codecpar->codec_id);

    ret = avcodec_parameters_to_context(codecCtx, pFormatCtx->streams[stream_index]->codecpar);
    if (ret < 0)
        return -1;
#endif

    if(codecCtx->codec_type == AVMEDIA_TYPE_AUDIO) {
        // Set audio settings from codec info
        wanted_spec.freq = codecCtx->sample_rate;
        wanted_spec.format = AUDIO_S16SYS;
        wanted_spec.channels = codecCtx->channels;
        wanted_spec.silence = 0;
        wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
        wanted_spec.callback = audio_callback;
        wanted_spec.userdata = is;

        if(SDL_OpenAudio(&wanted_spec, &spec) < 0) {
            fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
            return -1;
        }
    }
    if(avcodec_open2(codecCtx, codec, NULL) < 0) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }

    switch(codecCtx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->audioStream = stream_index;
        is->audio_st = pFormatCtx->streams[stream_index];
        is->audio_ctx = codecCtx;
        is->audio_buf_size = 0;
        is->audio_buf_index = 0;
        memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
        packet_queue_init(&is->audioq);
        SDL_PauseAudio(0);
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->videoStream = stream_index;
        is->video_st = pFormatCtx->streams[stream_index];
        is->video_ctx = codecCtx;

        // Make a screen to put our video
    #ifndef __DARWIN__
        screen = SDL_SetVideoMode(is->video_ctx->width, is->video_ctx->height, 0, 0);
    #else
        screen = SDL_SetVideoMode(is->video_ctx->width, is->video_ctx->height, 24, 0);
    #endif
        if(!screen) {
            fprintf(stderr, "SDL: could not set video mode - exiting\n");
            exit(1);
        }

        screen_mutex = SDL_CreateMutex();

        is->pictq_mutex = SDL_CreateMutex();
        is->pictq_cond = SDL_CreateCond();

        packet_queue_init(&is->videoq);
        is->video_tid = SDL_CreateThread(video_thread, is);
        is->sws_ctx = sws_getContext(is->video_ctx->width, is->video_ctx->height,
                                     is->video_ctx->pix_fmt, is->video_ctx->width,
                                     is->video_ctx->height, AV_PIX_FMT_YUV420P,
                                     SWS_BILINEAR, NULL, NULL, NULL
                                     );
        break;
    default:
        break;
    }

     return 0;
}

int decode_thread(void *arg) {

    VideoState *is = (VideoState *)arg;
    AVFormatContext *pFormatCtx=NULL;
    AVPacket pkt1, *packet = &pkt1;

    int video_index = -1;
    int audio_index = -1;
    int i;

    is->videoStream=-1;
    is->audioStream=-1;

    global_video_state = is;
    pFormatCtx=is->pFormatCtx;

    // Find the first video stream

    for(i=0; i<pFormatCtx->nb_streams; i++) {
        if(pFormatCtx->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO &&
                video_index < 0) {
            video_index=i;
        }
        if(pFormatCtx->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_AUDIO &&
                audio_index < 0) {
            audio_index=i;
        }
    }
    if(audio_index >= 0) {
        stream_component_open(is, audio_index);
    }
    if(video_index >= 0) {
        stream_component_open(is, video_index);
    }

    if(is->videoStream < 0 || is->audioStream < 0) {
        fprintf(stderr, "%s: could not open codecs\n", is->filename);
        goto fail;
    }

    // main decode loop

    for(;;) {
        if(is->quit) {
            break;
        }
        // seek stuff goes here
        // seek stuff goes here
        if(is->seek_req) {
            int stream_index= -1;
            int64_t seek_target = is->seek_pos;

            if     (is->videoStream >= 0) stream_index = is->videoStream;
            else if(is->audioStream >= 0) stream_index = is->audioStream;

            if(stream_index>=0){
                seek_target= av_rescale_q(seek_target, AV_TIME_BASE_Q,
                                          pFormatCtx->streams[stream_index]->time_base);
            }
            if(av_seek_frame(is->pFormatCtx, stream_index,
                             seek_target, is->seek_flags) < 0) {
                fprintf(stderr, "%s: error while seeking\n",
                        is->pFormatCtx->filename);
            } else {

                if(is->audioStream >= 0) {
                    packet_queue_flush(&is->audioq);
                    packet_queue_put(&is->audioq, &flush_pkt);
                }
                if(is->videoStream >= 0) {
                    packet_queue_flush(&is->videoq);
                    packet_queue_put(&is->videoq, &flush_pkt);
                }
            }
            is->seek_req = 0;
        }

        if(is->audioq.size > MAX_AUDIOQ_SIZE ||
                is->videoq.size > MAX_VIDEOQ_SIZE) {
            SDL_Delay(10);
            continue;
        }

        av_init_packet(packet);

        if(av_read_frame(is->pFormatCtx, packet) < 0) {
            if(is->pFormatCtx->pb->error == 0) {
                SDL_Delay(100); /* no error; wait for user input */
                continue;
            } else {
                break;
            }
        }
        // Is this a packet from the video stream?
        if(packet->stream_index == is->videoStream) {
            packet_queue_put(&is->videoq, packet);
        } else if(packet->stream_index == is->audioStream) {
            packet_queue_put(&is->audioq, packet);
        } else {
//            av_free_packet(packet);
            av_packet_unref(packet);
        }
    }
    /* all done - wait for it */
    while(!is->quit) {
        SDL_Delay(100);
    }

fail:
    if(1){
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    return 0;
}

void stream_seek(VideoState *is, int64_t pos, int rel) {

    if(!is->seek_req) {
        is->seek_pos = pos;
        is->seek_flags = rel < 0 ? AVSEEK_FLAG_BACKWARD : 0;
        is->seek_req = 1;
    }
}

double get_master_clock(VideoState *is) {
//    if(is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
//        return get_video_clock(is);
//    } else if(is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        return get_audio_clock(is);
//    } else {
//        return get_external_clock(is);
//    }
}

int main(int argc, char *argv[]) {

    SDL_Event       event;

    VideoState      *is;
    AVFormatContext *pFormatCtx=NULL;

    is = av_mallocz(sizeof(VideoState));

    is->audio_clock 				= 0.0;
    is->video_clock 				= 0.0;
    is->pre_frame_pts				= 0.0;		//前一帧显示时间
    is->pre_cur_frame_delay 		= 40e-3; 	//当前帧和前一帧的延时，前面两个相减的结果
    is->cur_frame_pts 				= 0.0;		//packet.pts
    is->delay 						= 40;

    if(argc < 2) {
        fprintf(stderr, "Usage: test <file>\n");
        exit(1);
    }
//    // Register all formats and codecs
//    av_register_all();

//    memset(is->filename, 0x0, 1024);
    av_strlcpy(is->filename, argv[1], sizeof(is->filename));
//    is->filename=argv[1];

    // Open video file
//    if(avformat_open_input(&pFormatCtx, is->filename, NULL, NULL)!=0)
    if(avformat_open_input(&pFormatCtx, is->filename, NULL, NULL)!=0)
        return -1; // Couldn't open file

    is->pFormatCtx = pFormatCtx;

    // Retrieve stream information
    if(avformat_find_stream_info(pFormatCtx, NULL)<0)
        return -1; // Couldn't find stream information

    // Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, is->filename, 0);

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }

    schedule_refresh(is, is->delay);

    is->parse_tid = SDL_CreateThread(decode_thread, is);
    if(!is->parse_tid) {
        av_free(is);
        return -1;
    }

    av_init_packet(&flush_pkt);
    flush_pkt.data = "FLUSH";

    for(;;) {
        double incr, pos;
        SDL_WaitEvent(&event);
        switch(event.type) {
        case SDL_KEYDOWN:
            switch(event.key.keysym.sym) {
            case SDLK_LEFT:
                incr = -10.0;
                goto do_seek;
            case SDLK_RIGHT:
                incr = 10.0;
                goto do_seek;
            case SDLK_UP:
                incr = 60.0;
                goto do_seek;
            case SDLK_DOWN:
                incr = -60.0;
                goto do_seek;
do_seek:
                if(global_video_state) {
                    pos = get_master_clock(global_video_state);
                    pos += incr;
                    stream_seek(global_video_state, (int64_t)(pos * AV_TIME_BASE), incr);
                }
                break;
            default:
                break;
            }
            break;

        case FF_QUIT_EVENT:
        case SDL_QUIT:
            is->quit = 1;
            /*
       * If the video has finished playing, then both the picture and
       * audio queues are waiting for more data.  Make them stop
       * waiting and terminate normally.
       */
            SDL_CondSignal(is->audioq.cond);
            SDL_CondSignal(is->videoq.cond);

            SDL_Quit();
            return 0;
            break;
        case FF_REFRESH_EVENT:
            video_refresh_timer(event.user.data1);
            break;
        default:
            break;
        }
    }
    return 0;
}
