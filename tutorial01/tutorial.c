#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
#include <stdio.h>

void printHelpMenu();
void saveFrame(AVFrame* avFrame, int width, int height, int frameIndex);

int main(int argc, char* argv[]) {
    if (argc != 3) {
        printHelpMenu();
        return -1;
    }
    AVFormatContext* pFormatCtx = NULL;
    // 打开视频文件, 并且初始化
    int ret = avformat_open_input(&pFormatCtx, argv[1], NULL, NULL);
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

    int i;
    AVCodecContext* pCodecCtxOrig = NULL;
    AVCodecContext* pCodecCtx = NULL;

    int videoStream = -1; // 找到第一个视频流
    for (i = 0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream = i;
            break;
        }
     }
    if (videoStream == -1) {
        return -1;
    }
    // 获取解码器
    AVCodec* pCodec = NULL;
    pCodec = avcodec_find_decoder(pFormatCtx->streams[videoStream]->codecpar->codec_id);
    if (pCodec == NULL) {
        printf("avcodec_find_decoder failed\n");
        return -1;
    }
    // 分配和复制编解码器上下文
    pCodecCtxOrig = pFormatCtx->streams[videoStream]->codec;
    // pCodecCtxOrig = avcodec_alloc_context3(pCodec);
    // // 将AVCodecContext的成员复制到AVCodecParameters
    // ret = avcodec_parameters_to_context(pCodecCtxOrig, pFormatCtx->streams[videoStream]->codecpar);
    // if (ret < 0) {
    //     printf("avcodec_parameters_to_context failed\n");
    //     return -1;
    // }

    // 注意一点, 我们千万不要直接使用来自于视频流的AVCodecContext!
    // 所以我们需要去拷贝这个上下文到一个新的上下文到一个新的地方:
    // ret = avcodec_copy_context(pCodecCtx, pCodecCtxOrig);
    pCodecCtx = avcodec_alloc_context3(pCodec);
    ret = avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[videoStream]->codecpar);
    if (ret < 0) {
        printf("Could not copy codec context.\n");
        return -1;
    }


    // 打开解码器
    ret = avcodec_open2(pCodecCtx, pCodec, NULL);
    if (ret < 0) {
        printf("avcodec_open2 failed\n");
        return -1;
    }

    // 现在我们需要一个地方去存放解码后的帧
    AVFrame* pFrame = NULL;
    pFrame = av_frame_alloc();
    if (pFrame == NULL) {
        printf("av_frame_alloc failed\n");
        return -1;
    }
    // 分配RGB帧
    // 因为我们想要输出ppm文件, 它实际上存的是24bit的RGB. 我们需要从它原始格式转换我们的帧为RGB. 但是ffmpeg 会自动帮我们做转换.
    // 我们来创建一个帧给转换帧存放:
    AVFrame* pFrameRGB = NULL;
    pFrameRGB = av_frame_alloc(); // 分配一个帧, 等会用于存储RGB帧
    if (pFrameRGB == NULL) {
        printf("av_frame_alloc failed\n");
        return -1;
    }
    // 即使我们已经创建好帧了, 当我们在转换过程中时, 仍然需要一个地方保存原始(Raw)数据.
    // 我们使用avpicture_get_size 去获取我们需要的buffer大小.
    uint8_t* buffer = NULL;
    int numBytes;

    // numBytes = avpicture_get_size(AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height);
    numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height, 32);
    buffer = (uint8_t*)av_malloc(numBytes*sizeof(uint8_t));

    // 填充RGB帧 -> 将缓冲区与pFrameRGB 关联.
    // avpicture_fill((AVPicture*)pFrameRGB, buffer, AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height);
    av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize, buffer, AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height, 32);

    // 初始化缩放上下文

    // 终于, 我们现在可以从流中读取数据.
    // 接下来我们要做的就是把一整个视频流从包(packet)中读取出来, 解码到我们得帧, 然后只要帧完毕, 我们就会转化它并且保存它.
    struct SwsContext* sws_ctx = NULL;
    AVPacket* pPacket = av_packet_alloc();
    if (pPacket == NULL) {
        printf("av_packet_alloc failed\n");
        return -1;
    }
    // 创建一个缩放的上下文
    sws_ctx = sws_getContext(
        pCodecCtx->width, pCodecCtx->height,
        pCodecCtx->pix_fmt,
        pCodecCtx->width, pCodecCtx->height,
        AV_PIX_FMT_RGB24,
        SWS_BILINEAR, NULL, NULL, NULL
    );

    int maxFramesToDecode;
    sscanf(argv[2], "%d", &maxFramesToDecode);
    // 读取和解码帧
    i = 0;
    while (av_read_frame(pFormatCtx, pPacket) >= 0) {
        // 读取一个包, 是否来自视频流?
        if (pPacket->stream_index == videoStream) {
            // 解码视频流
            // avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &pPacket);
            // Deprecated! Use avcodec_send_packet() and avcodec_receive_frame().
            ret = avcodec_send_packet(pCodecCtx, pPacket);
            if (ret < 0) {
                printf("avcodec_send_packet failed\n");
                return -1;
            }
            printf("av_read_frame read packet: %d\n", ret);
            while (ret >= 0) {
                // 循环解码包
                // 也许有多个帧, 确保每个帧都处理过后再开始读取下一个包
                ret = avcodec_receive_frame(pCodecCtx, pFrame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    printf("avcodec_receive_frame failed\n");
                    return -1;
                }
                // 缩放帧
                sws_scale(sws_ctx, (uint8_t const* const*)pFrame->data, pFrame->linesize, 0, pCodecCtx->height, pFrameRGB->data, pFrameRGB->linesize);
                if (++i <= maxFramesToDecode) {
                    saveFrame(pFrameRGB, pCodecCtx->width, pCodecCtx->height, i);
                    // 打印日志信息:
                    // printf("Frame: %c(%d) pts %d dts %d key_frame %d "
                    // "[coded_picture_number %d, display_picture_number %d,"
                    // " %dx%d]\n",
                    //     av_get_picture_type_char(pFrame->pict_type),
                    //     pCodecCtx->frame_number,
                    //     pFrameRGB->pts,
                    //     pFrameRGB->pkt_dts,
                    //     pFrameRGB->key_frame,
                    //     pFrameRGB->coded_picture_number,
                    //     pFrameRGB->display_picture_number,
                    //     pCodecCtx->width,
                    //     pCodecCtx->height
                    // );
                } else {
                    break;
                }
            }
            if (i > maxFramesToDecode) {
                break;
            }
        }
        av_packet_unref(pPacket);
    }
    // cleanup:
    // Free RGB image
    av_free(buffer);
    av_frame_free(&pFrameRGB);
    av_free(pFrameRGB);
    // Free YUV frame
    av_frame_free(&pFrame);
    av_free(pFrame);

    // Close the codecs
    avcodec_close(pCodecCtx);
    avcodec_close(pCodecCtxOrig);

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

void saveFrame(AVFrame* avFrame, int width, int height, int i) {
    FILE* pf;
    char szFilename[32];
    int y;
    sprintf(szFilename, "tmp/frame%d.ppm", i);
    pf = fopen(szFilename, "wb");
    if (pf == NULL) return;

    fprintf(pf, "P6\n%d %d\n255\n", width, height);
    for (y = 0; y < height; y++) {
        fwrite(avFrame->data[0] + y*avFrame->linesize[0], 1, width*3, pf);
    }
    fclose(pf);
}