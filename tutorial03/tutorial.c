#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>


#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#include <stdio.h>
#include <assert.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 48000*2*2 // (48khz) * 16bit * 2channel

typedef struct PacketQueue {
    AVPacketList* first_pkt;
    AVPacketList* last_pkt;
    int nb_packets;
    int size;
    SDL_mutex* mutex;
    SDL_cond* cond;
} PacketQueue;

PacketQueue audioq;

void printHelpMenu();
void saveFrame(AVFrame* avFrame, int width, int height, int frameIndex);
void audio_callback(void* userdata, Uint8* stream, int len);
void packet_queue_init(PacketQueue *q);
int packet_queue_put(PacketQueue* queue, AVPacket* packet);
static int audio_resampling(
    AVCodecContext* audio_decode_ctx, AVFrame* decoded_audio_frame,
    enum AVSampleFormat out_sample_fmt, int out_channels, int out_sample_rate, uint8_t* out_buf
);
static int packet_queue_get(PacketQueue* q, AVPacket* pkt, int block);

// global quit flag
int quit = 0;
int main(int argc, char* argv[]) {
    if (argc != 3) {
        printHelpMenu();
        return -1;
    }
    int maxFramesToDecode;
    sscanf(argv[2], "%d", &maxFramesToDecode);

    int ret = -1;
    ret = SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_TIMER);
    if (ret < 0) {
        printf("SDL_Init failed\n");
        return -1;
    }

    AVFormatContext* pFormatCtx = NULL;
    // 打开视频文件, 并且初始化
    ret = avformat_open_input(&pFormatCtx, argv[1], NULL, NULL);
    if (ret < 0) {
        printf("avformat_open_input failed\n");
        return -1;
    }
    // 获取视频信息
    ret = avformat_find_stream_info(pFormatCtx, NULL);
    if (ret < 0) {
        printf("avformat_find_stream_info failed\n");
        return -1;
    }
    // find之后, 能找到视频流数据, 然后为每个视频流数据增加codecpar -> pFormatCtx->streams

    // 打印视频信息
    av_dump_format(pFormatCtx, 0, argv[1], 0);

    int videoStream = -1; // 找到第一个视频流
    int audioStream = -1;
    for (int i = 0; i < pFormatCtx->nb_streams; i++) {
        // look for video stream
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
          videoStream < 0) {
          videoStream = i;
        }

        // look for audio stream
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
            audioStream < 0) {
          audioStream = i;
        }
    }
    if (videoStream == -1) {
        return -1;
    }
    if (audioStream == -1) {
      printf("Could not find audio stream.\n");
      return -1;
    }
    // 获取音频解码器
    AVCodec* aCodec = NULL;
    aCodec = avcodec_find_decoder(pFormatCtx->streams[audioStream]->codecpar->codec_id);
    if (aCodec == NULL) {
        printf("avcodec_find_decoder failed\n");
        return -1;
    }
    // 获取音频解码器上下文
    AVCodecContext* aCodecCtx = NULL;
    // 用于分配一个AVCodecContext结构体, 并将其初始化为指定的AVCodec(即编解码器)兼容的状态
    aCodecCtx = avcodec_alloc_context3(aCodec);
    ret = avcodec_parameters_to_context(aCodecCtx, pFormatCtx->streams[audioStream]->codecpar);
    if (ret < 0) {
        printf("avcodec_parameters_to_context audio failed\n");
        return -1;
    }
    SDL_AudioSpec wanted_specs;
    SDL_AudioSpec specs;

    wanted_specs.freq = aCodecCtx->sample_rate;
    wanted_specs.format = AUDIO_S16SYS;
    wanted_specs.channels = aCodecCtx->channels;
    wanted_specs.silence = 0;
    wanted_specs.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_specs.callback = audio_callback;
    wanted_specs.userdata = aCodecCtx;

    SDL_AudioDeviceID audioDeviceID;

    audioDeviceID = SDL_OpenAudioDevice(
      NULL, 0, &wanted_specs, &specs, SDL_AUDIO_ALLOW_FORMAT_CHANGE
    );
    if (audioDeviceID == 0) {
      printf("Failed to open audio device: %s\n", SDL_GetError());
      return -1;
    }
    // 打开音频解码器
    ret = avcodec_open2(aCodecCtx, aCodec, NULL);
    if (ret < 0) {
        printf("avcodec_open2 failed\n");
        return -1;
    }
    packet_queue_init(&audioq);

    // 开启播放音频到给定的音频设备
    SDL_PauseAudioDevice(audioDeviceID, 0);


    AVCodec* pCodec = NULL;
    pCodec = avcodec_find_decoder(pFormatCtx->streams[videoStream]->codecpar->codec_id);
    if (!pCodec) {
        printf("avcodec_find_decoder failed\n");
        return -1;
    }

    AVCodecContext* pCodecCtx = NULL;
    pCodecCtx = avcodec_alloc_context3(pCodec);
    // 将AVCodecContext的成员复制到AVCodecParameters中
    ret = avcodec_parameters_to_context(
        pCodecCtx, pFormatCtx->streams[videoStream]->codecpar
    );
    if (ret < 0) {
        printf("avcodec_parameters_to_context failed\n");
        return -1;
    }
    ret = avcodec_open2(pCodecCtx, pCodec, NULL);
    if (ret < 0) {
        printf("avcodec_open2 failed\n");
        return -1;
    }

    AVFrame* pFrame = av_frame_alloc();
    SDL_Window* screen = SDL_CreateWindow(
        "Hello World", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        pCodecCtx->width / 3, pCodecCtx->height / 3, SDL_WINDOW_OPENGL|SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (!screen) {
        printf("SDL_CreateWindow failed\n");
    }
    SDL_GL_SetSwapInterval(1);

    SDL_Renderer* render = NULL;
    render = SDL_CreateRenderer(screen, -1, SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC|SDL_RENDERER_TARGETTEXTURE);

    SDL_Texture* texture = NULL;
    texture = SDL_CreateTexture(render, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, pCodecCtx->width, pCodecCtx->height);

    AVPacket* pPacket = av_packet_alloc();
    if (pPacket == NULL) {
        printf("av_packet_alloc error\n");
        return -1;
    }
    struct SwsContext* sws_ctx = NULL;
    sws_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height,
        pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height,
        AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL
    );

    int numBytes;
    numBytes = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height, 32);

    uint8_t* buffer = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));
    // 上面的frame用于保存传输用的帧
    // 这个frame用于保存解码后的帧
    AVFrame* pict = av_frame_alloc();
    // 将buffer缓冲区与pict帧关联
    av_image_fill_arrays(pict->data, pict->linesize, buffer, AV_PIX_FMT_YUV420P,
        pCodecCtx->width, pCodecCtx->height, 32
    );


    SDL_Event event;
    int frameIndex = 0;

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
                    printf("avcodec_receive_frame error\n");
                    return -1;
                }
                sws_scale(sws_ctx, (uint8_t const* const*)pFrame->data, pFrame->linesize, 0, pCodecCtx->height,
                    pict->data, pict->linesize);
                if (++frameIndex <= maxFramesToDecode) {
                    SDL_Rect rect;
                    rect.x = 0;
                    rect.y = 0;
                    rect.w = aCodecCtx->width;
                    rect.h = aCodecCtx->height;
                    SDL_UpdateYUVTexture(texture, &rect,
                        pict->data[0], pict->linesize[0], // Y
                        pict->data[1], pict->linesize[1], // U
                        pict->data[2], pict->linesize[2] // V
                    );
                    // 清楚渲染目标
                    SDL_RenderClear(render);
                    // Copy a portion of the texture to the current rendering target.
                    SDL_RenderCopy(render, texture, NULL, NULL);
                    SDL_RenderPresent(render);
                }
            }
            if (frameIndex > maxFramesToDecode) {
                printf("Max frames to decode reached\n");
                break;
            }
        } else if (pPacket->stream_index == audioStream) {
            packet_queue_put(&audioq, pPacket);
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
        }
        if (quit) {
            break;
        }
    }

    // cleanup:
    av_packet_unref(pPacket);
    // Free RGB image
    av_free(buffer);
    av_frame_free(&pict);
    av_free(pict);
    // Free YUV frame
    av_frame_free(&pFrame);
    av_free(pFrame);

    // Close the codecs
    avcodec_close(pCodecCtx);
    avcodec_close(aCodecCtx);

    avformat_close_input(&pFormatCtx);

    return 0;
}

void printHelpMenu() {
    printf("Invalid arguments.\n\n");
    printf("Usage: ./tutorial01 <filename> <max-frames-to-decode>\n\n");
    printf(
        "e.g: ./tutorial01 /home/rambodrahmani/Videos/Labrinth-Jealous.mp4 "
        "200\n");
}

void audio_callback(void* userdata, Uint8* stream, int len) {
  AVCodecContext* aCodecCtx = userdata;

  int len1 = -1;
  int audio_size = -1;
  // 设置一个1.5倍最大预估ffmpeg给出的音频帧大小的缓冲数据大小, 这个是一个很好的缓冲数值
  static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
  static unsigned int audio_buf_size = 0, audio_buf_index = 0;

  while (len > 0) {
    if (quit) {
      return;
    }
    if (audio_buf_index >= audio_buf_size) {
      audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf));
      if (audio_size < 0) {
        // 发送silence声音
        audio_buf_size = 1024;
        memset(audio_buf, 0, audio_buf_size);
        puts("audio_decode_frame() failed");
      } else {
        audio_buf_size = audio_size;
      }
      audio_buf_index = 0;
    }
    len1 = audio_buf_size - audio_buf_index;
    if (len1 > len) {
      len1 = len;
    }
    // 将音频数据拷贝到SDL播放的缓冲区中
    memcpy(stream, (uint8_t*)audio_buf+audio_buf_index, len1);
    len -= len1;
    stream += len1;
    audio_buf_index += len1;
  }
}

void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));

    q->mutex = SDL_CreateMutex();
    if (!q->mutex) {
        puts("SDL_CreateMutex() failed");
        exit(1);
    }
    q->cond = SDL_CreateCond();
    if (!q->cond) {
        puts("SDL_CreateCond() failed");
        exit(1);
    }
}
int packet_queue_put(PacketQueue* queue, AVPacket* packet) {
    AVPacketList* avPacketList;
    avPacketList = av_malloc(sizeof(AVPacketList));
    if (!avPacketList) {
        return -1;
    }
    avPacketList->pkt = *packet;
    avPacketList->next = NULL;
    SDL_LockMutex(queue->mutex);
    // 判别queue是否为空
    if (!queue->last_pkt) {
        queue->first_pkt = avPacketList;
    } else {
        queue->last_pkt->next = avPacketList;
    }
    queue->last_pkt = avPacketList;
    queue->nb_packets++;
    queue->size += avPacketList->pkt.size;
    // packet_queue_get 函数中wait 条件变量，当有数据时，会唤醒，否则阻塞
    SDL_CondSignal(queue->cond);
    SDL_UnlockMutex(queue->mutex);
}

int audio_decode_frame(AVCodecContext* aCodecCtx, uint8_t* audio_buf, int buf_size) {
    AVPacket* avPacket = av_packet_alloc();
    static uint8_t* audio_pkt_data = NULL;
    static int audio_pkt_size = 0;

    // 生成一个新的帧, 用来解析音频的包
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
            // len1 = avcodec_decode_audio4(aCodecCtx, avFrame, &got_frame, avPacket);
            int ret = avcodec_receive_frame(aCodecCtx, avFrame);
            if (ret == 0) {
                got_frame = 1;
            }
            if (ret == AVERROR(EAGAIN)) {
                ret = 0;
            }
            if (ret ==0) {
                ret = avcodec_send_packet(aCodecCtx, avPacket);
            }
            if (ret == AVERROR(EAGAIN)) {
                ret = 0;
            } else if (ret < 0) {
                printf("avcodec_receive_frame error\n");
                return -1;
            } else {
                len1 = avPacket->size;
            }

            if (len1 < 0) {
                audio_pkt_size = 0;
                break;
            }
            audio_pkt_data += len1;
            audio_pkt_size -= len1;
            data_size = 0;

            if (got_frame) {
                data_size = audio_resampling(aCodecCtx, avFrame, AV_SAMPLE_FMT_S16,
                    aCodecCtx->channels, aCodecCtx->sample_rate, audio_buf
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
    enum AVSampleFormat out_sample_fmt, int out_channels, int out_sample_rate, uint8_t* out_buf
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
        printf("swr_alloc error\n");
        return -1;
    }
    if (audio_decode_ctx->channels == av_get_channel_layout_nb_channels(audio_decode_ctx->channel_layout)) {
        in_channel_layout = audio_decode_ctx->channel_layout;
    } else {
        in_channel_layout = av_get_default_channel_layout(audio_decode_ctx->channels);
    }
    if (in_channel_layout <= 0) {
        printf("in_channel_layout error\n");
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
        printf("in_nb_samples error\n");
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
        printf("swr_init error\n");
        return -1;
    }
    max_out_nb_samples = out_nb_samples =
        av_rescale_rnd(in_nb_samples, out_sample_rate, audio_decode_ctx->sample_rate, AV_ROUND_UP);
    if (max_out_nb_samples <= 0) {
        printf("av_rescale_rnd error\n");
        return -1;
    }

    out_nb_channels = av_get_channel_layout_nb_channels(out_channel_layout);
    ret = av_samples_alloc_array_and_samples(
        &resampled_data, &out_linesize, out_nb_channels, out_nb_samples, out_sample_fmt, 0);
    if (ret < 0) {
        printf("av_samples_alloc_array_and_samples error\n");
        return -1;
    }
    out_nb_samples = av_rescale_rnd(
        swr_get_delay(swr_ctx, audio_decode_ctx->sample_rate) + in_nb_samples,
        out_sample_rate, audio_decode_ctx->sample_rate, AV_ROUND_UP
    );
    if (out_nb_samples <= 0) {
        printf("out_nb_samples <= 0\n");
        return -1;
    }
    if (out_nb_samples > max_out_nb_samples) {
        av_free(resampled_data[0]);
        ret = av_samples_alloc(resampled_data, &out_linesize, out_nb_channels,
            out_nb_samples, out_sample_fmt, 1);
        if (ret <0) {
            printf("av_samples_alloc error\n");
            return -1;
        }
        max_out_nb_samples = out_nb_samples;
    }
    if (swr_ctx) {
        ret = swr_convert(swr_ctx, resampled_data, out_nb_samples,
            (const uint8_t **)decoded_audio_frame->data, decoded_audio_frame->nb_samples
        );
        if (ret < 0) {
            printf("swr_convert error\n");
            return -1;
        }
        resampled_data_size = av_samples_get_buffer_size(
            &out_linesize, out_nb_channels, ret, out_sample_fmt, 1
        );
        if (resampled_data_size < 0) {
            printf("av_samples_get_buffer_size error\n");
            return -1;
        }
    } else {
        printf("swr_ctx null error.\n");
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