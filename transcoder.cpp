#include "transcoder.hpp"

#include <array>
#include <cerrno>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/defs.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace {

[[nodiscard]] std::string ffmpegError(const int code)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(code, buffer.data(), buffer.size());
    return std::string(buffer.data());
}

void check(const int code, const std::string_view operation)
{
    if (code < 0) {
        throw std::runtime_error(
            std::string(operation) + ": " + ffmpegError(code));
    }
}

struct InputFormatDeleter {
    void operator()(AVFormatContext* context) const noexcept
    {
        if (context != nullptr) {
            avformat_close_input(&context);
        }
    }
};

struct OutputFormatDeleter {
    void operator()(AVFormatContext* context) const noexcept
    {
        if (context == nullptr) {
            return;
        }

        if (context->pb != nullptr &&
            (context->oformat->flags & AVFMT_NOFILE) == 0) {
            avio_closep(&context->pb);
        }

        avformat_free_context(context);
    }
};

struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const noexcept
    {
        if (context != nullptr) {
            avcodec_free_context(&context);
        }
    }
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const noexcept
    {
        if (frame != nullptr) {
            av_frame_free(&frame);
        }
    }
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const noexcept
    {
        if (packet != nullptr) {
            av_packet_free(&packet);
        }
    }
};

struct SwsContextDeleter {
    void operator()(SwsContext* context) const noexcept
    {
        sws_freeContext(context);
    }
};

struct SwrContextDeleter {
    void operator()(SwrContext* context) const noexcept
    {
        if (context != nullptr) {
            swr_free(&context);
        }
    }
};

using InputFormatPtr = std::unique_ptr<AVFormatContext, InputFormatDeleter>;
using OutputFormatPtr = std::unique_ptr<AVFormatContext, OutputFormatDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;
using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;

} // namespace

void transcodeToDnXHR(
    const std::filesystem::path& inputPath,
    const std::filesystem::path& outputPath)
{
    AVFormatContext* rawInput = nullptr;
    check(
        avformat_open_input(
            &rawInput,
            inputPath.string().c_str(),
            nullptr,
            nullptr),
        "open input");
    InputFormatPtr input(rawInput);

    check(
        avformat_find_stream_info(input.get(), nullptr),
        "find stream info");

    const int videoIndex = av_find_best_stream(
        input.get(),
        AVMEDIA_TYPE_VIDEO,
        -1,
        -1,
        nullptr,
        0);

    if (videoIndex < 0) {
        throw std::runtime_error("input has no video stream");
    }

    const int audioIndex = av_find_best_stream(
        input.get(),
        AVMEDIA_TYPE_AUDIO,
        -1,
        -1,
        nullptr,
        0);

    AVStream* inputVideo = input->streams[videoIndex];

    const AVCodec* decoder =
        avcodec_find_decoder(inputVideo->codecpar->codec_id);
    if (decoder == nullptr) {
        throw std::runtime_error("video decoder not found");
    }

    CodecContextPtr decoderContext(avcodec_alloc_context3(decoder));
    if (decoderContext == nullptr) {
        throw std::runtime_error("cannot allocate video decoder");
    }

    check(
        avcodec_parameters_to_context(
            decoderContext.get(),
            inputVideo->codecpar),
        "copy decoder parameters");
    check(
        avcodec_open2(decoderContext.get(), decoder, nullptr),
        "open video decoder");

    CodecContextPtr audioDecoderContext;
    if (audioIndex >= 0) {
        AVStream* inputAudio = input->streams[audioIndex];
        const AVCodec* audioDecoder =
            avcodec_find_decoder(inputAudio->codecpar->codec_id);
        if (audioDecoder == nullptr) {
            throw std::runtime_error("audio decoder not found");
        }

        audioDecoderContext.reset(avcodec_alloc_context3(audioDecoder));
        if (audioDecoderContext == nullptr) {
            throw std::runtime_error("cannot allocate audio decoder");
        }

        check(
            avcodec_parameters_to_context(
                audioDecoderContext.get(),
                inputAudio->codecpar),
            "copy audio decoder parameters");
        check(
            avcodec_open2(
                audioDecoderContext.get(),
                audioDecoder,
                nullptr),
            "open audio decoder");

        if (audioDecoderContext->ch_layout.nb_channels <= 0 ||
            audioDecoderContext->sample_rate <= 0) {
            throw std::runtime_error("audio stream has invalid format");
        }
    }

    AVFormatContext* rawOutput = nullptr;
    check(
        avformat_alloc_output_context2(
            &rawOutput,
            nullptr,
            "mov",
            outputPath.string().c_str()),
        "create output context");
    if (rawOutput == nullptr) {
        throw std::runtime_error("cannot create MOV output context");
    }
    OutputFormatPtr output(rawOutput);

    const AVCodec* encoder = avcodec_find_encoder_by_name("dnxhd");
    if (encoder == nullptr) {
        throw std::runtime_error("DNxHR encoder not found");
    }

    CodecContextPtr encoderContext(avcodec_alloc_context3(encoder));
    if (encoderContext == nullptr) {
        throw std::runtime_error("cannot allocate DNxHR encoder");
    }

    AVRational frameRate = av_guess_frame_rate(
        input.get(),
        inputVideo,
        nullptr);
    if (frameRate.num <= 0 || frameRate.den <= 0) {
        frameRate = {25, 1};
    }

    encoderContext->width = decoderContext->width;
    encoderContext->height = decoderContext->height;
    encoderContext->time_base = av_inv_q(frameRate);
    encoderContext->framerate = frameRate;
    encoderContext->profile = AV_PROFILE_DNXHR_HQ;
    encoderContext->pix_fmt = AV_PIX_FMT_YUV422P;
    encoderContext->max_b_frames = 0;
    encoderContext->sample_aspect_ratio =
        inputVideo->sample_aspect_ratio.num > 0 &&
                inputVideo->sample_aspect_ratio.den > 0
            ? inputVideo->sample_aspect_ratio
            : AVRational{1, 1};

    if ((output->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        encoderContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // FFmpeg 的 dnxhd 编码器把 profile 定义为私有 AVOption；
    // 只设置 AVCodecContext::profile 会被编码器忽略。
    check(
        av_opt_set(
            encoderContext->priv_data,
            "profile",
            "dnxhr_hq",
            0),
        "set DNxHR profile");

    check(
        avcodec_open2(encoderContext.get(), encoder, nullptr),
        "open DNxHR encoder");

    AVStream* outputVideo = avformat_new_stream(output.get(), nullptr);
    if (outputVideo == nullptr) {
        throw std::runtime_error("cannot create output video stream");
    }
    outputVideo->time_base = encoderContext->time_base;
    outputVideo->avg_frame_rate = frameRate;
    outputVideo->sample_aspect_ratio = encoderContext->sample_aspect_ratio;
    check(
        avcodec_parameters_from_context(
            outputVideo->codecpar,
            encoderContext.get()),
        "copy encoder parameters");
    outputVideo->codecpar->codec_tag = 0;

    // DNxHR 只重新编码视频；音频解码后编码为 PCM s16le。
    AVStream* outputAudio = nullptr;
    CodecContextPtr audioEncoderContext;
    SwrContextPtr resampler;
    if (audioIndex >= 0) {
        const AVCodec* audioEncoder =
            avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
        if (audioEncoder == nullptr) {
            throw std::runtime_error("PCM s16le encoder not found");
        }

        audioEncoderContext.reset(avcodec_alloc_context3(audioEncoder));
        if (audioEncoderContext == nullptr) {
            throw std::runtime_error("cannot allocate PCM encoder");
        }

        audioEncoderContext->sample_rate =
            audioDecoderContext->sample_rate;
        audioEncoderContext->sample_fmt = AV_SAMPLE_FMT_S16;
        audioEncoderContext->time_base = {
            1,
            audioEncoderContext->sample_rate
        };
        check(
            av_channel_layout_copy(
                &audioEncoderContext->ch_layout,
                &audioDecoderContext->ch_layout),
            "copy audio channel layout");

        if ((output->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
            audioEncoderContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        check(
            avcodec_open2(
                audioEncoderContext.get(),
                audioEncoder,
                nullptr),
            "open PCM encoder");

        outputAudio = avformat_new_stream(output.get(), nullptr);
        if (outputAudio == nullptr) {
            throw std::runtime_error("cannot create output audio stream");
        }

        check(
            avcodec_parameters_from_context(
                outputAudio->codecpar,
                audioEncoderContext.get()),
            "copy PCM encoder parameters");
        outputAudio->codecpar->codec_tag = 0;
        outputAudio->time_base = audioEncoderContext->time_base;

        SwrContext* rawResampler = nullptr;
        check(
            swr_alloc_set_opts2(
                &rawResampler,
                &audioEncoderContext->ch_layout,
                audioEncoderContext->sample_fmt,
                audioEncoderContext->sample_rate,
                &audioDecoderContext->ch_layout,
                audioDecoderContext->sample_fmt,
                audioDecoderContext->sample_rate,
                0,
                nullptr),
            "create audio resampler");
        resampler.reset(rawResampler);
        check(swr_init(resampler.get()), "initialize audio resampler");
    }

    if ((output->oformat->flags & AVFMT_NOFILE) == 0) {
        check(
            avio_open(
                &output->pb,
                outputPath.string().c_str(),
                AVIO_FLAG_WRITE),
            "open output file");
    }

    check(avformat_write_header(output.get(), nullptr), "write header");

    SwsContextPtr scaler(sws_getContext(
        decoderContext->width,
        decoderContext->height,
        decoderContext->pix_fmt,
        encoderContext->width,
        encoderContext->height,
        encoderContext->pix_fmt,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr));
    if (scaler == nullptr) {
        throw std::runtime_error("cannot create video pixel converter");
    }

    FramePtr decodedFrame(av_frame_alloc());
    FramePtr convertedFrame(av_frame_alloc());
    FramePtr decodedAudioFrame(
        audioIndex >= 0 ? av_frame_alloc() : nullptr);
    PacketPtr inputPacket(av_packet_alloc());
    PacketPtr outputPacket(av_packet_alloc());
    if (!decodedFrame || !convertedFrame || !inputPacket ||
        !outputPacket || (audioIndex >= 0 && !decodedAudioFrame)) {
        throw std::runtime_error("cannot allocate FFmpeg frame or packet");
    }

    convertedFrame->format = encoderContext->pix_fmt;
    convertedFrame->width = encoderContext->width;
    convertedFrame->height = encoderContext->height;
    check(av_frame_get_buffer(convertedFrame.get(), 32), "allocate video frame");

    auto writeEncodedFrames = [&](AVFrame* frame) {
        check(
            avcodec_send_frame(encoderContext.get(), frame),
            "send video frame to encoder");

        while (true) {
            const int result = avcodec_receive_packet(
                encoderContext.get(),
                outputPacket.get());

            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                break;
            }
            check(result, "receive encoded video packet");

            av_packet_rescale_ts(
                outputPacket.get(),
                encoderContext->time_base,
                outputVideo->time_base);
            outputPacket->stream_index = outputVideo->index;

            check(
                av_interleaved_write_frame(output.get(), outputPacket.get()),
                "write video packet");
            av_packet_unref(outputPacket.get());
        }
    };

    auto writeEncodedAudio = [&](AVFrame* frame) {
        if (audioEncoderContext == nullptr || outputAudio == nullptr) {
            return;
        }

        check(
            avcodec_send_frame(audioEncoderContext.get(), frame),
            "send audio frame to encoder");

        while (true) {
            const int result = avcodec_receive_packet(
                audioEncoderContext.get(),
                outputPacket.get());

            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                break;
            }
            check(result, "receive encoded audio packet");

            av_packet_rescale_ts(
                outputPacket.get(),
                audioEncoderContext->time_base,
                outputAudio->time_base);
            outputPacket->stream_index = outputAudio->index;

            check(
                av_interleaved_write_frame(output.get(), outputPacket.get()),
                "write audio packet");
            av_packet_unref(outputPacket.get());
        }
    };

    int64_t nextFramePts = 0;
    int64_t nextAudioPts = 0;

    auto convertAudioFrame = [&](const AVFrame* frame) {
        if (audioEncoderContext == nullptr || resampler == nullptr) {
            return;
        }

        const int inputSamples = frame == nullptr ? 0 : frame->nb_samples;
        const int64_t delayedSamples = swr_get_delay(
            resampler.get(),
            audioDecoderContext->sample_rate);
        const int64_t outputSamples64 = av_rescale_rnd(
            delayedSamples + inputSamples,
            audioEncoderContext->sample_rate,
            audioDecoderContext->sample_rate,
            AV_ROUND_UP);

        if (outputSamples64 <= 0) {
            return;
        }
        if (outputSamples64 > std::numeric_limits<int>::max()) {
            throw std::runtime_error("audio frame is too large");
        }

        FramePtr convertedAudioFrame(av_frame_alloc());
        if (convertedAudioFrame == nullptr) {
            throw std::runtime_error("cannot allocate converted audio frame");
        }

        convertedAudioFrame->format = audioEncoderContext->sample_fmt;
        convertedAudioFrame->sample_rate = audioEncoderContext->sample_rate;
        convertedAudioFrame->nb_samples =
            static_cast<int>(outputSamples64);
        check(
            av_channel_layout_copy(
                &convertedAudioFrame->ch_layout,
                &audioEncoderContext->ch_layout),
            "copy converted audio channel layout");
        check(
            av_frame_get_buffer(convertedAudioFrame.get(), 0),
            "allocate converted audio samples");

        const int convertedSamples = swr_convert(
            resampler.get(),
            convertedAudioFrame->data,
            convertedAudioFrame->nb_samples,
            frame == nullptr ? nullptr : frame->extended_data,
            inputSamples);
        check(convertedSamples, "resample audio");

        if (convertedSamples == 0) {
            return;
        }

        convertedAudioFrame->nb_samples = convertedSamples;
        convertedAudioFrame->pts = nextAudioPts;
        nextAudioPts += convertedSamples;
        writeEncodedAudio(convertedAudioFrame.get());
    };

    auto decodeVideoPacket = [&](const AVPacket* packet) {
        check(
            avcodec_send_packet(decoderContext.get(), packet),
            "send input video packet");

        while (true) {
            const int result = avcodec_receive_frame(
                decoderContext.get(),
                decodedFrame.get());

            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                break;
            }
            check(result, "decode video frame");

            check(
                av_frame_make_writable(convertedFrame.get()),
                "make converted frame writable");

            sws_scale(
                scaler.get(),
                decodedFrame->data,
                decodedFrame->linesize,
                0,
                decoderContext->height,
                convertedFrame->data,
                convertedFrame->linesize);

            if (decodedFrame->best_effort_timestamp == AV_NOPTS_VALUE) {
                convertedFrame->pts = nextFramePts++;
            } else {
                convertedFrame->pts = av_rescale_q(
                    decodedFrame->best_effort_timestamp,
                    inputVideo->time_base,
                    encoderContext->time_base);
                nextFramePts = convertedFrame->pts + 1;
            }

            writeEncodedFrames(convertedFrame.get());
            av_frame_unref(decodedFrame.get());
        }
    };

    auto decodeAudioPacket = [&](const AVPacket* packet) {
        if (audioDecoderContext == nullptr) {
            return;
        }

        check(
            avcodec_send_packet(audioDecoderContext.get(), packet),
            "send input audio packet");

        while (true) {
            const int result = avcodec_receive_frame(
                audioDecoderContext.get(),
                decodedAudioFrame.get());

            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                break;
            }
            check(result, "decode audio frame");

            convertAudioFrame(decodedAudioFrame.get());
            av_frame_unref(decodedAudioFrame.get());
        }
    };

    while (av_read_frame(input.get(), inputPacket.get()) >= 0) {
        if (inputPacket->stream_index == videoIndex) {
            decodeVideoPacket(inputPacket.get());
        } else if (inputPacket->stream_index == audioIndex) {
            decodeAudioPacket(inputPacket.get());
        }

        av_packet_unref(inputPacket.get());
    }

    // 刷新视频解码器和编码器，写出最后缓存的帧。
    decodeVideoPacket(nullptr);
    decodeAudioPacket(nullptr);
    if (resampler != nullptr) {
        while (swr_get_delay(
                   resampler.get(),
                   audioDecoderContext->sample_rate) > 0) {
            convertAudioFrame(nullptr);
        }
    }
    writeEncodedFrames(nullptr);
    writeEncodedAudio(nullptr);

    check(av_write_trailer(output.get()), "write trailer");
}
