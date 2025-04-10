#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>
#include <assert.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <stdio.h>
#include <unistd.h>

// 一般设置音频缓存大小为1024byte
#define SDL_AUDIO_BUFFER_SIZE 1024
// 一般设置音频最大缓存大小方案: (48khz) * 2(16bit) * 2channel
// 48000 * 2byte * 2c
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
void packet_queue_init(PacketQueue *q);
int packet_queue_put(PacketQueue *queue, AVPacket *packet);
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block);
void audio_callback(void *userdata, Uint8 *stream, int len);
int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf,
                       int buf_size);
int audio_resampling(AVCodecContext *audio_decode_ctx,
                            AVFrame *audio_decode_frame,
                            enum AVSampleFormat out_sample_fmt,
                            int out_channels, int out_sample_rate,
                            uint8_t *out_buf);

int main(int argc, char **argv) {
    int ret = -1;

    AVFormatContext* pFormatCtx = NULL;
    ret = avformat_open_input(&pFormatCtx, argv[1], NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open input file '%s'\n", argv[1]);
        exit(1);
    }
    ret = avformat_find_stream_info(pFormatCtx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not find stream information\n");
        exit(1);
    }
    av_dump_format(pFormatCtx, 0, argv[1], 0);

    int audioStream = -1;
    for (int i = 0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audioStream < 0) {
            audioStream = i;
            break;
        }
    }
    if (audioStream < 0) {
        fprintf(stderr, "Could not find audio stream\n");
        exit(1);
    }
    AVCodec* aCodec = avcodec_find_decoder(pFormatCtx->streams[audioStream]->codecpar->codec_id);
    AVCodecContext* aCodecCtx = avcodec_alloc_context3(aCodec);
    ret = avcodec_parameters_to_context(aCodecCtx, pFormatCtx->streams[audioStream]->codecpar);
    if (ret < 0) {
        fprintf(stderr, "Could not copy codec parameters to codec context\n");
        exit(1);
    }
    ret = avcodec_open2(aCodecCtx, aCodec, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(1);
    }
    packet_queue_init(&audioq);
    // 开始设置SDL音频相关配置
    ret = SDL_Init(SDL_INIT_AUDIO|SDL_INIT_TIMER);
    if (ret < 0) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }
    SDL_AudioSpec wanted_spec, spec;
    wanted_spec.freq = aCodecCtx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = aCodecCtx->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = aCodecCtx;

    SDL_AudioDeviceID deviceID = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FORMAT_CHANGE);
    if (deviceID == 0) {
        fprintf(stderr, "Could not open audio: %s\n", SDL_GetError());
        exit(1);
    }
    SDL_PauseAudioDevice(deviceID, 0);
    // frame_size 每帧样本数
    // 一帧大小(byte) = 每帧样本数 * 每样本字节数 * 声道数
    // 一帧时间(s) = 每帧样本数 / 采样率
    uint64_t constantly_delay_ms = 0;
    constantly_delay_ms = aCodecCtx->frame_size * 1000 / aCodecCtx->sample_rate;
    printf("delay for %d ms\n", constantly_delay_ms);
    if (constantly_delay_ms > 16) {
        // 不能直接设置为16, 否则音频还没播放完, 就停止了;
        // 不能直接设置为23(上面计算的), 否则每帧播放中间有停顿;
        // 暂时减1, 减少停顿
        constantly_delay_ms -= 1;
    }

    AVPacket* pPacket = av_packet_alloc();
    SDL_Event event;
    while (av_read_frame(pFormatCtx, pPacket) >= 0) {
        if (pPacket->stream_index == audioStream) {
            packet_queue_put(&audioq, pPacket);
            SDL_Delay(constantly_delay_ms);
        } else {
            av_packet_unref(pPacket);
        }
        SDL_PollEvent(&event);
        switch (event.type) {
        case SDL_QUIT:
            SDL_Quit();
            quit = 1;
            break;
        }
        if (quit) {
            break;
        }
    }
    av_packet_unref(pPacket);
    avcodec_close(aCodecCtx);
    avformat_close_input(&pFormatCtx);
}

void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *queue, AVPacket *packet) {
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
    SDL_LockMutex(queue->mutex);
    if (!queue->last_pkt) {
        queue->first_pkt = avPacketList;
    } else {
        // 将之前设置的最后一个avpacketList (包括first_pkt)设置链接next
        queue->last_pkt->next = avPacketList;
    }
    // 默认将当前的对象放到最后一个item中.
    queue->last_pkt = avPacketList;
    queue->nb_packets++;
    queue->size += avPacketList->pkt.size;
    SDL_CondSignal(queue->cond);
    SDL_UnlockMutex(queue->mutex);
    return 0;
}
void audio_callback(void *userdata, Uint8 *stream, int len) {
    AVCodecContext* aCodecCtx = (AVCodecContext*)userdata;

    int len1 = -1;
    int audio_size = -1;
    // 音频缓冲大小1.5倍的最大音频帧大小, 这让ffmpeg一个合适的缓冲区间
    static uint8_t audio_buf[MAX_AUDIO_FRAME_SIZE*3/2];
    static unsigned int audio_buf_size = 0;
    static unsigned int audio_buf_index = 0;
    while (len > 0) {
        if (quit) {
            break;
        }
        if (audio_buf_index >= audio_buf_size) {
            audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf));
            if (audio_size < 0) {
                audio_buf_size = 1024;
                memset(audio_buf, 0, audio_buf_size);
                printf("audio_decode_frame error\n");
            } else {
                audio_buf_size = audio_size;
            }
            audio_buf_index = 0;
        }
        len1 = audio_buf_size - audio_buf_index;
        if (len1 > len) {
            len1 = len;
        }
        memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);

        len -= len1;
        stream += len1;
        audio_buf_index += len1;
    }
}
int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf,
    int buf_size) {
    AVPacket* avPacket = av_packet_alloc();
    static uint8_t* audio_pkt_data = NULL;
    static int audio_pkt_size = 0;

    static AVFrame* avFrame = NULL;
    avFrame = av_frame_alloc();
    if (avFrame == NULL) {
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
                puts("Error while decoding");
                return -1;
            } else {
                len1 = avPacket->size;
            }
            if (len1 < 0) {
                audio_pkt_size = 0;
                break;
            }
            // 首次len1 = 0, 所以+/-没有影响
            audio_pkt_data += len1;
            audio_pkt_size -= len1;
            data_size = 0;
            if (got_frame) {
                data_size = audio_resampling(aCodecCtx, avFrame, AV_SAMPLE_FMT_S16,
                    aCodecCtx->channels, aCodecCtx->sample_rate, audio_buf);
                assert(data_size <= buf_size);
            }
            // 第一次avcodec_send_packet还未收到got_frame, 进入continue
            // 第二次avcodec_receive_frame收到got_frame, 进入if
            if (data_size <= 0) {
                // no data yet, get more frames
                continue;
            }
            // 活的到数据则直接返回
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

int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
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
        }  else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}
int audio_resampling(AVCodecContext* aCodecCtx, AVFrame* avFrame,
    enum AVSampleFormat out_sample_fmt, int out_channels, int out_sample_rate, uint8_t* out_buf) {
    if (quit) return -1;

    int ret = 0, resampled_data_size = 0;
    int64_t in_channel_layout = aCodecCtx->channel_layout;
    int64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
    int out_nb_channels = 0, out_linesize = 0, out_nb_samples = 0, max_out_nb_samples = 0;
    int in_nb_samples = 0;
    uint8_t** resampled_data = NULL;

    SwrContext* swr_ctx = swr_alloc();
    if (!swr_ctx) {
        fprintf(stderr, "Could not allocate resampler context\n");
        return -1;
    }
    if (aCodecCtx->channels == av_get_channel_layout_nb_channels(aCodecCtx->channel_layout)) {
        in_channel_layout = aCodecCtx->channel_layout;
    } else {
        in_channel_layout = av_get_default_channel_layout(aCodecCtx->channels);
    }
    // check input audio channels correctly retrieved
    if (in_channel_layout <= 0) {
        printf("in_channel_layout error.\n");
        return -1;
    }

    // set output audio channels based on the input audio channels
    if (out_channels == 1) {
        out_channel_layout = AV_CH_LAYOUT_MONO;
    } else if (out_channels == 2) {
        out_channel_layout = AV_CH_LAYOUT_STEREO;
    } else {
        out_channel_layout = AV_CH_LAYOUT_SURROUND;
    }
    in_nb_samples = avFrame->nb_samples;
    if (in_nb_samples <= 0) {
        printf("in_nb_samples error.\n");
        return -1;
    }
    av_opt_set_int(swr_ctx, "in_channel_layout", in_channel_layout, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate", aCodecCtx->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", aCodecCtx->sample_fmt, 0);
    av_opt_set_int(swr_ctx, "out_channel_layout", out_channel_layout, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate", out_sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", out_sample_fmt, 0);

    // Once all values have been set for the SwrContext, it must be initialized
    // with swr_init().
    ret = swr_init(swr_ctx);
    if (ret < 0) {
        fprintf(stderr, "Error initializing the resampling context\n");
        return -1;
    }
    max_out_nb_samples = out_nb_samples =
      av_rescale_rnd(in_nb_samples, out_sample_rate, aCodecCtx->sample_rate, AV_ROUND_UP);
    if (max_out_nb_samples <= 0) {
        printf("av_rescale_rnd error.\n");
        return -1;
    }

    out_nb_channels = av_get_channel_layout_nb_channels(out_channel_layout);
    ret = av_samples_alloc_array_and_samples(&resampled_data,
        &out_linesize, out_nb_channels, out_nb_samples, out_sample_fmt, 0);
    if (ret < 0) {
        fprintf(stderr, "Error allocating the resampler data buffers\n");
        return -1;
    }
    out_nb_samples = av_rescale_rnd(
        swr_get_delay(swr_ctx, aCodecCtx->sample_rate)+in_nb_samples,
        out_sample_rate, aCodecCtx->sample_rate, AV_ROUND_UP
    );
    if (out_nb_samples <= 0) {
        fprintf(stderr, "Could not allocate out_nb_samples\n");
        return -1;
    }
    if (out_nb_samples > max_out_nb_samples) {
        av_free(resampled_data[0]);
        ret = av_samples_alloc(resampled_data, &out_linesize, out_nb_channels,
                              out_nb_samples, out_sample_fmt, 1);
        if (ret < 0) {
            fprintf(stderr, "Could not allocate out_nb_samples\n");
            return -1;
        }
        max_out_nb_samples = out_nb_samples;
    }
    if (swr_ctx) {
        ret = swr_convert(swr_ctx, resampled_data, out_nb_samples,
                   (const uint8_t **)avFrame->data, avFrame->nb_samples);
        if (ret < 0) {
            fprintf(stderr, "swr_convert() failed\n");
            return -1;
        }
        resampled_data_size = av_samples_get_buffer_size(
            &out_linesize, out_nb_channels, ret, out_sample_fmt, 1
        );
        if (resampled_data_size < 0) {
            fprintf(stderr, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
    } else {
        puts("swr_ctx error");
        return -1;
    }
    memcpy(out_buf, resampled_data[0], resampled_data_size);
    if (resampled_data) {
        // free memory block and set pointer to NULL
        av_freep(&resampled_data[0]);
    }

    av_freep(&resampled_data);
    resampled_data = NULL;

    if (swr_ctx) {
        // Free the given SwrContext and set the pointer to NULL
        swr_free(&swr_ctx);
    }
    return resampled_data_size;
}