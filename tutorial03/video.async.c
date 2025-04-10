#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>
#include <assert.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <unistd.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
// 一般设置音频最大缓存大小方案: (48khz) * 2(16bit) * 2channel = 192000
#define MAX_AUDIO_FRAME_SIZE 192000

typedef struct PacketQueue {
    AVPacketList* first_pkt;
    AVPacketList* last_pkt;
    int nb_packets;
    int size;
    SDL_mutex* mutex;
    SDL_cond* cond;
} PacketQueue;

PacketQueue audioq;
int quit = 0;

void packet_queue_init(PacketQueue* q);
int packet_queue_put(PacketQueue* q, AVPacket* packet);
static int packet_queue_get(PacketQueue* q, AVPacket* packet, int block);
void audio_callback(void* userdata, Uint8* stream, int len);
int audio_decode_frame(AVCodecContext* aCodecContext, uint8_t* audio_buf, int buf_size);
static int audio_resampling(
    AVCodecContext* audio_decode_ctx,
    AVFrame* audio_decode_frame,
    enum AVSampleFormat out_sample_fmt,
    int out_channels, int out_sample_rate,
    uint8_t* out_buf
);

int main(int argc, char* argv[]) {
    if(argc < 2) {
        return -1;
    }

    int ret = -1;
    AVFormatContext* pFormatCtx = NULL;
    ret = avformat_open_input(&pFormatCtx, argv[1], NULL, NULL);
    if (ret < 0) {
        printf("Could not open source file %s\n", argv[1]);
        return -1;
    }
    ret = avformat_find_stream_info(pFormatCtx, NULL);
    if (ret < 0) {
        printf("Could not find stream information\n");
        return -1;
    }
    av_dump_format(pFormatCtx, 0, argv[1], 0);
    int videoStream = -1, audioStream = -1;
    for (int i = 0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoStream < 0) {
            videoStream = i;
        }
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audioStream < 0) {
            audioStream = i;
        }
    }
    if (videoStream < 0 || audioStream < 0) {
        printf("Could not find video or audio stream.\n");
        return -1;
    }
    // 找到视音频解码器
    AVCodec* aCodec = avcodec_find_decoder(pFormatCtx->streams[audioStream]->codecpar->codec_id);
    if (!aCodec) {
        printf("Unsupported codec\n");
        return -1;
    }
    // 根据解码器创建解码器上下文
    // AVCodecContext* aCodecOriginCtx = pFormatCtx->streams[audioStream]->codec;
    // ret = avcodec_copy_context(aCodecCtx, aCodecOriginCtx);
    AVCodecContext* aCodecCtx = avcodec_alloc_context3(aCodec);
    ret = avcodec_parameters_to_context(aCodecCtx, pFormatCtx->streams[audioStream]->codecpar);
    if (ret < 0) {
        printf("Could not copy codec parameters to decoder context\n");
        return -1;
    }

    // 打开解码器
    // 初始化音频的AVCodecContext 去使用对应的解码器
    ret = avcodec_open2(aCodecCtx, aCodec, NULL);
    if (ret < 0) {
        printf("Could not open codec\n");
        return -1;
    }
    packet_queue_init(&audioq);

    AVCodec* pCodec = avcodec_find_decoder(pFormatCtx->streams[videoStream]->codecpar->codec_id);
    AVCodecContext* pCodecCtx = avcodec_alloc_context3(pCodec);
    // 将AVCodecParameters 的成员复制到AVCodecContext中
    ret = avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[videoStream]->codecpar);
    if (ret < 0) {
        printf("Could not copy codec parameters to decoder context\n");
        return -1;
    }
    // 使用上下文打开解码器
    ret = avcodec_open2(pCodecCtx, pCodec, NULL);
    AVFrame* pFrame = av_frame_alloc();

    ret = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER);
    if (ret < 0) {
        printf("Could not initialize SDL - %s\n", SDL_GetError());
        return -1;
    }
    SDL_AudioSpec wanted_specs;
    SDL_AudioSpec specs;
    // 从解码器的信息去设置音频参数
    wanted_specs.freq = aCodecCtx->sample_rate;
    wanted_specs.format = AUDIO_S16SYS;
    wanted_specs.channels = aCodecCtx->channels;
    wanted_specs.silence = 0;
    wanted_specs.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_specs.callback = audio_callback;
    wanted_specs.userdata = aCodecCtx;

    SDL_AudioDeviceID audioDeviceID = SDL_OpenAudioDevice(NULL, 0, &wanted_specs, &specs, SDL_AUDIO_ALLOW_FORMAT_CHANGE);
    if (audioDeviceID == 0) {
        printf("Could not open audio device - %s\n", SDL_GetError());
        return -1;
    }
    SDL_PauseAudioDevice(audioDeviceID, 0);

    // Graphic
    SDL_Window* screen = SDL_CreateWindow("FFMPEG", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        pCodecCtx->width/4, pCodecCtx->height/4, SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!screen) {
        puts("SDL_CreateWindow failed!");
        return -1;
    }
    SDL_GL_SetSwapInterval(1);
    SDL_Renderer* renderer = SDL_CreateRenderer(screen, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, pCodecCtx->width, pCodecCtx->height);
    struct SwsContext* sws_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height, 32);
    uint8_t* buffer = (uint8_t*)av_malloc(numBytes*sizeof(uint8_t));
    // 这个frame用于保存video解码后的帧
    AVFrame* pict = av_frame_alloc();
    // 将buffer缓冲区与pict帧关联
    av_image_fill_arrays(pict->data, pict->linesize, buffer, AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height, 32);

    AVPacket* pPacket = av_packet_alloc();
    if (!pPacket) {
        printf("Could not allocate AVPacket\n");
        return -1;
    }
    SDL_Event event;
    while (av_read_frame(pFormatCtx, pPacket) >= 0) {
        if (pPacket->stream_index == videoStream) {
            // 使用pPacket接收一个包的数据
            ret = avcodec_send_packet(pCodecCtx, pPacket);
            if (ret < 0) {
                printf("avcodec_send_packet error\n");
                return -1;
            }
            while (ret >= 0) {
                // 从pPacket包中接收其中一个帧的数据放进pFrame中
                ret = avcodec_receive_frame(pCodecCtx, pFrame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    puts("avcodec_receive_frame error");
                    return -1;
                }
                sws_scale(sws_ctx, (uint8_t const* const*)pFrame->data, pFrame->linesize, 0, pCodecCtx->height, pict->data, pict->linesize);
                SDL_Rect rect;
                rect.x = 0; rect.y = 0; rect.w = pCodecCtx->width; rect.h = pCodecCtx->height;
                SDL_UpdateYUVTexture(texture, &rect,
                    pict->data[0], pict->linesize[0], pict->data[1], pict->linesize[1], pict->data[2], pict->linesize[2]);
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, NULL, NULL);
                SDL_RenderPresent(renderer);
            }
        } else {
            av_packet_unref(pPacket);
        }
        SDL_PollEvent(&event);
        switch (event.type) {
            case SDL_QUIT: {
                printf("Quit\n");
                SDL_Quit();
                quit = 1;
            }
                break;
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_SPACE) {
                    printf("Quit\n");
                    SDL_Quit();
                    quit = 1;
                }
                break;
        }
        if (quit) {
            break;
        }
    }
    av_packet_unref(pPacket);

    // Free RGB image
    av_free(buffer);
    av_frame_free(&pict);
    av_free(pict);
    // Free YUV frame
    av_frame_free(&pFrame);
    av_free(pFrame);

    avcodec_close(pCodecCtx);
    avcodec_close(aCodecCtx);

    avformat_close_input(&pFormatCtx);
    return 0;
}

void packet_queue_init(PacketQueue* q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    if (!q->mutex) {
        printf("SDL_CreateMutex error\n");
        return;
    }
    q->cond = SDL_CreateCond();
    if (!q->cond) {
        printf("SDL_CreateCond error\n");
        return;
    }
}

int packet_queue_put(PacketQueue* q, AVPacket* packet) {
    AVPacketList* avPacketList = av_malloc(sizeof(AVPacketList));
    if (!avPacketList) {
        return -1;
    }
    avPacketList->pkt = *packet;
    avPacketList->next = NULL;

    /** 1.
     * PacketQueue q:
        +-----------------------------+
        | first_pkt: NULL            |
        | last_pkt: NULL             |
        | nb_packets: 0              |
        | size: 0                   |
        | mutex: (SDL_mutex*)        |
        | cond: (SDL_cond*)          |
        +-----------------------------+
        first_pkt 和 last_pkt 都为 NULL，表示队列为空。
     */
    /** 2.
     * PacketQueue q:
    +-----------------------------+
    | first_pkt: -->[packet1]    |
    | last_pkt: -->[packet1]     |
    | nb_packets: 1              |
    | size: packet1.size        |
    | mutex: (SDL_mutex*)        |
    | cond: (SDL_cond*)          |
    +-----------------------------+

    链表结构:
    [packet1]--> NULL
     */
    /** 3.
     * PacketQueue q:
    +-----------------------------+
    | first_pkt: -->[packet1]    |
    | last_pkt: -->[packet2]     |
    | nb_packets: 2              |
    | size: packet1.size + packet2.size |
    | mutex: (SDL_mutex*)        |
    | cond: (SDL_cond*)          |
    +-----------------------------+

    链表结构:
    [packet1]-->[packet2]--> NULL
     */
    /** 4.
     * PacketQueue q:
    +-----------------------------+
    | first_pkt: -->[packet1]    |
    | last_pkt: -->[packet3]     |
    | nb_packets: 3              |
    | size: packet1.size + packet2.size + packet3.size |
    | mutex: (SDL_mutex*)        |
    | cond: (SDL_cond*)          |
    +-----------------------------+

    链表结构:
    [packet1]-->[packet2]-->[packet3]--> NULL
     */
    SDL_LockMutex(q->mutex);
    if (!q->last_pkt) {
        q->first_pkt = avPacketList;
    } else {
        // 将之前设置的最后一个avpacketList (包括first_pkt)设置链接next
        q->last_pkt->next = avPacketList;
    }
    // 默认将当前的对象放到最后一个item中.
    q->last_pkt = avPacketList;
    q->nb_packets++;
    q->size += avPacketList->pkt.size;
    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);

    return 0;
}
void audio_callback(void* userdata, Uint8* stream, int len) {
    AVCodecContext* aCodecCtx = (AVCodecContext*)userdata;

    int len1 = -1;
    int audio_size = -1;
    // 音频缓冲大小1.5倍的最大音频帧大小, 这让ffmpeg一个合适的缓冲区间
    static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
    static unsigned int audio_buf_index = 0;
    static unsigned int audio_buf_size = 0;
    // when ask for a len of frame:
    while (len > 0) {
        if (quit) {
            return;
        }
        if (audio_buf_index >= audio_buf_size) {
            audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf));
            if (audio_size < 0) {
                audio_buf_size = 1024;

                memset(audio_buf, 0, audio_buf_size);
                printf("audio_decode_frame() failed.\n");
            } else {
                audio_buf_size = audio_size;
            }
            audio_buf_index = 0;
        }
        len1 = audio_buf_size - audio_buf_index;
        if (len1 > len) {
            len1 = len;
        }
        memcpy(stream, (uint8_t*)audio_buf + audio_buf_index, len1);
        len -= len1;
        stream += len1;
        audio_buf_index += len1;
    }
}

int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size) {
    AVPacket* avPacket = av_packet_alloc();
    static uint8_t* audio_pkt_data = NULL;
    static int audio_pkt_size = 0;

    static AVFrame* avFrame = NULL;
    avFrame = av_frame_alloc();
    if (!avFrame) {
        printf("av_frame_alloc error\n");
        return -1;
    }

    int len1 = 0;
    int data_size = 0;

    for (;;) {
        if (quit) {
            return -1;
        }
        while (audio_pkt_size > 0) {
            int got_frame = 0;
            int ret = avcodec_receive_frame(aCodecCtx, avFrame);
            if (ret == 0) {
                got_frame = 1;
            }
            // 首次获取数据报AVERROR(EAGAIN), 所以进入avcodec_send_packet
            if (ret == AVERROR(EAGAIN)) {
                ret = 0;
            }
            if (ret == 0) {
                ret = avcodec_send_packet(aCodecCtx, avPacket);
            }
            if (ret == AVERROR(EAGAIN)) {
                ret = 0;
            } else if (ret < 0) {
                printf("Error in avcodec_receive_frame error\n");
                return -1;
            } else {
                len1 = avPacket->size;
            }
            // 如果没有获取数据(avPacket->size),
            // 则跳出循环, 等待下次的packet_queue_get
            if (len1 < 0) {
                audio_pkt_size = 0;
                break;
            }
            // 首次len1 = 0, 所以可以+/-
            audio_pkt_data += len1;
            audio_pkt_size -= len1;
            data_size = 0;
            // 第一次avcodec_send_packet还未收到got_frame, 进入continue
            // 第二次avcodec_receive_frame收到got_frame, 进入if
            if (got_frame) {
                data_size = audio_resampling(aCodecCtx, avFrame, AV_SAMPLE_FMT_S16,
                                            aCodecCtx->channels,
                                            aCodecCtx->sample_rate, audio_buf
                );
                assert(data_size <= buf_size);
            }
            if (data_size <= 0) {
                continue;
            }
            return data_size;
        }
        if (avPacket->data) {
            av_packet_unref(avPacket);
        }

        int ret = packet_queue_get(&audioq, avPacket, 1);
        if (ret < 0) {
            return -1;
        }
        audio_pkt_data = avPacket->data;
        audio_pkt_size = avPacket->size;
    }
    return 0;

}

static int packet_queue_get(PacketQueue* q, AVPacket* pkt, int block) {
    int ret;
    AVPacketList* avPacketList;

    SDL_LockMutex(q->mutex);
    for (;;) {
        if (quit) {
            ret = -1;
            break;
        }
        avPacketList = q->first_pkt;
        if (avPacketList) {
            q->first_pkt = avPacketList->next;
            if (!q->first_pkt) {
                q->last_pkt = NULL;
            }
            q->nb_packets--;
            q->size -= avPacketList->pkt.size;
            *pkt = avPacketList->pkt;
            av_free(avPacketList);

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

static int audio_resampling(
    AVCodecContext* audio_decode_ctx, AVFrame* decoded_audio_frame,
    enum AVSampleFormat out_sample_fmt, int out_channels, int out_sample_rate,
    uint8_t* out_buf
) {
    if (quit) {
        return -1;
    }
    SwrContext* swr_ctx = NULL;
    int ret = 0;
    int64_t in_channel_layout = audio_decode_ctx->channel_layout;
    int64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
    int out_nb_channels = 0;
    int out_linesize = 0;
    int in_nb_samples = 0;
    int out_nb_samples = 0;
    int max_out_nb_samples = 0;
    uint8_t** resampled_data = NULL;
    int resampled_data_size = 0;

    swr_ctx = swr_alloc();
    if (!swr_ctx) {
        fprintf(stderr, "Could not allocate resampler context\n");
        exit(1);
    }
    if (audio_decode_ctx->channels == av_get_channel_layout_nb_channels(audio_decode_ctx->channel_layout)) {
        in_channel_layout = audio_decode_ctx->channel_layout;
    } else {
        in_channel_layout = av_get_default_channel_layout(audio_decode_ctx->channels);
    }
    if (in_channel_layout <= 0) {
        fprintf(stderr, "Could not set input channel layout\n");
        return -1;
    }

    if (out_channels == 1) {
        out_channel_layout = AV_CH_LAYOUT_MONO;
    } else if (out_channels == 2) {
        out_channel_layout = AV_CH_LAYOUT_STEREO;
    } else {
        out_channel_layout = AV_CH_LAYOUT_SURROUND;
    }

    in_nb_samples = decoded_audio_frame->nb_samples;
    if (in_nb_samples <= 0) {
        printf("Could not get input samples\n");
        return -1;
    }
    av_opt_set_int(swr_ctx, "in_channel_layout", in_channel_layout, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate", audio_decode_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", audio_decode_ctx->sample_fmt, 0);
    av_opt_set_int(swr_ctx, "out_channel_layout", out_channel_layout, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate", out_sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", out_sample_fmt, 0);
    ret = swr_init(swr_ctx);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to initialize the resampling context\n");
        return;
    }
    // 算出最大的输出样本数
    // a * b / c
    max_out_nb_samples = out_nb_samples = av_rescale_rnd(in_nb_samples, out_sample_rate, audio_decode_ctx->sample_rate, AV_ROUND_UP);
    if (max_out_nb_samples <= 0) {
        printf("max_out_nb_samples <= 0\n");
        return -1;
    }
    // get number of output audio channels
    out_nb_channels = av_get_channel_layout_nb_channels(out_channel_layout);
    // Allocate a data pointers array, samples buffer for nb_samples
    // samples, and fill data pointers and linesize accordingly.
    ret = av_samples_alloc_array_and_samples(&resampled_data, &out_linesize,
                                            out_nb_channels, out_nb_samples,
                                            out_sample_fmt, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate resampler data: %s\n",
                av_err2str(ret));
        return -1;
    }
    out_nb_samples = av_rescale_rnd(
        swr_get_delay(swr_ctx, audio_decode_ctx->sample_rate)+in_nb_samples, out_sample_rate,
        audio_decode_ctx->sample_rate,
        AV_ROUND_UP
    );
    if (out_nb_samples <= 0) {
        fprintf(stderr, "Could not allocate out_nb_samples\n");
        return -1;
    }
    if (out_nb_samples > max_out_nb_samples) {
        av_free(resampled_data[0]);
        ret = av_samples_alloc(
            resampled_data, &out_linesize, out_nb_channels,
            out_nb_samples, out_sample_fmt, 1
        );
        if (ret < 0) {
            printf("av_samples_alloc fail\n");
            return -1;
        }
        max_out_nb_samples = out_nb_channels;
    }
    if (swr_ctx) {
        ret = swr_convert(
            swr_ctx, resampled_data, out_nb_samples,
            (const uint8_t **)decoded_audio_frame->data, decoded_audio_frame->nb_samples
        );
        if (ret < 0) {
            printf("swr_convert fail\n");
            return -1;
        }
        resampled_data_size = av_samples_get_buffer_size(
            &out_linesize, out_nb_channels, ret, out_sample_fmt, 1
        );
        if (resampled_data_size < 0) {
            printf("av_samples_get_buffer_size fail\n");
            return -1;
        }
    } else {
        printf("swr_ctx null error!\n");
        return -1;
    }
    memcpy(out_buf, resampled_data[0], resampled_data_size);

    if (resampled_data) {
        av_freep(&resampled_data[0]);
    }
    av_freep(&resampled_data);
    resampled_data = NULL;
    if (swr_ctx) {
        swr_free(&swr_ctx);
    }
    return resampled_data_size;
}
