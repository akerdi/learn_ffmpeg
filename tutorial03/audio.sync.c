#include <stdio.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <SDL2/SDL.h>

#define MAX_AUDIO_FRAME_SIZE 192000

static Uint8* audio_chunk;
static Uint32 audio_len;
static Uint8* audio_pos;

void audio_callback(void* userdata, Uint8* stream, int len) {
    // SDL_memset(stream, 0, len);
    if (audio_len == 0) {
        memset(stream, 0, len);
        return;
    }
    len = (len > audio_len ? audio_len : len);
    memcpy(stream, audio_pos, len);
    audio_pos += len;
    audio_len -= len;
}

int main(int argc, char** argv) {
    AVFormatContext* pFormatCtx;
    int i, audioStream;
    AVCodecContext* aCodecCtx;
    const AVCodec* aCodec;
    AVPacket* pPacket;
    AVFrame* pFrame;
    SDL_AudioSpec wanted_spec, specs;
    int ret;
    uint32_t len = 0;
    int got_picture;
    int index = 0;
    int64_t in_channel_layout;
    struct SwrContext* au_convert_ctx;
    int quit = 0;

    pFormatCtx = avformat_alloc_context();
    if (avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) != 0) {
        printf("Couldn't open input stream.\n");
        return -1;
    }
    if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
        printf("Couldn't find stream information.\n");
        return -1;
    }
    av_dump_format(pFormatCtx, 0, argv[1], 0);
    audioStream = -1;
    for (i = 0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStream = i;
            break;
        }
    }
    if (audioStream == -1) {
        printf("Didn't find a audio stream.\n");
        return -1;
    }
    aCodec = avcodec_find_decoder(pFormatCtx->streams[audioStream]->codecpar->codec_id);
    if (aCodec == NULL) {
        printf("aCodec not found.\n");
        return -1;
    }
    aCodecCtx = avcodec_alloc_context3(aCodec);
    ret = avcodec_parameters_to_context(aCodecCtx, pFormatCtx->streams[audioStream]->codecpar);
    if (ret < 0) {
        printf("avcodec_parameters_to_context error.\n");
        return -1;
    }
    ret = avcodec_open2(aCodecCtx, aCodec, NULL);
    if (ret < 0) {
        printf("avcodec_open2 error.\n");
        return -1;
    }
    pPacket = av_packet_alloc();

    uint64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
    uint64_t out_channels = aCodecCtx->channels;
    if (out_channels == 1) {
        out_channel_layout = AV_CH_LAYOUT_MONO;
    } else if (out_channels == 2) {
        out_channel_layout = AV_CH_LAYOUT_STEREO;
    } else {
        out_channel_layout = AV_CH_LAYOUT_SURROUND;
    }
    int out_nb_samples = aCodecCtx->frame_size;
    enum AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;
    int out_sample_rate = aCodecCtx->sample_rate;

    int out_buffer_size = av_samples_get_buffer_size(NULL, out_channels, out_nb_samples, out_sample_fmt, 1);
    printf("out_channels=%d, out_channel_layout=%d, out_nb_samples=%d, out_sample_fmt=%d, out_sample_rate=%d, out_buffer_size=%d\n",
        out_channels,   out_channel_layout,     out_nb_samples,     out_sample_fmt,     out_sample_rate, out_buffer_size);
    if (out_buffer_size ==0) {
        printf("av_samples_get_buffer_size error\n");
        return -1;
    }
    uint8_t* out_buffer = (uint8_t*)av_malloc(MAX_AUDIO_FRAME_SIZE*3/2);
    pFrame = av_frame_alloc();

    if (SDL_Init(SDL_INIT_AUDIO|SDL_INIT_TIMER)) {
        printf("Could not initialize SDL - %s\n", SDL_GetError());
        return -1;
    }
    wanted_spec.freq = out_sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = out_channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = out_nb_samples;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = aCodecCtx;

    SDL_AudioDeviceID audioDeviceID = SDL_OpenAudioDevice(
        NULL, 0, &wanted_spec, &specs, SDL_AUDIO_ALLOW_FORMAT_CHANGE
    );

    if (audioDeviceID == 0) {
        printf("Could not open audio device - %s\n", SDL_GetError());
        return -1;
    }
    if (aCodecCtx->channels == av_get_channel_layout_nb_channels(aCodecCtx->channel_layout)) {
        in_channel_layout = aCodecCtx->channel_layout;
    } else {
        in_channel_layout = av_get_default_channel_layout(aCodecCtx->channels);
    }
    if (in_channel_layout <= 0) {
        fprintf(stderr, "Could not set input channel layout\n");
        return -1;
    }

    au_convert_ctx = swr_alloc();
    au_convert_ctx = swr_alloc_set_opts(
        au_convert_ctx,
        out_channel_layout, out_sample_fmt, out_sample_rate,
        in_channel_layout, aCodecCtx->sample_fmt, aCodecCtx->sample_rate,
        0, NULL
    );
    swr_init(au_convert_ctx);
    // SDL_PauseAudio(0);
    SDL_PauseAudioDevice(audioDeviceID, 0);
    SDL_Event event;
    while (av_read_frame(pFormatCtx, pPacket) >= 0) {
        if (pPacket->stream_index == audioStream) {
            ret = avcodec_send_packet(aCodecCtx, pPacket);
            if (ret < 0) continue;

            got_picture = avcodec_receive_frame(aCodecCtx, pFrame);
            if (got_picture == 0) {
                swr_convert(au_convert_ctx, &out_buffer, MAX_AUDIO_FRAME_SIZE, (const uint8_t**)pFrame->data, pFrame->nb_samples);
                index++;
            }
            int64_t delay_ms = swr_get_delay(au_convert_ctx, aCodecCtx->sample_rate);
            printf("delay_ms: %ld\n", delay_ms);
            if (delay_ms <= 0) delay_ms = 23.2;
            SDL_Delay(delay_ms);
            // while (audio_len > 0)
            //     SDL_Delay(1);
            audio_chunk = (Uint8*)out_buffer;
            audio_len = out_buffer_size;
            audio_pos = audio_chunk;
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
                        quit = 1;
                    }
                    break;
            }
            if (quit) {
                break;
            }
        }
        av_packet_unref(pPacket);
    }
    swr_free(&au_convert_ctx);
    SDL_CloseAudio();
    SDL_Quit();
    av_free(out_buffer);
    avcodec_close(aCodecCtx);
    avformat_close_input(&pFormatCtx);

    return 0;
}