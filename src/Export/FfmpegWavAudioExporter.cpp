#include "AveMediaBridge/Export/FfmpegWavAudioExporter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
}

namespace AveMediaBridge {
namespace {

std::string ffErrorString(int err) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buffer, sizeof(buffer));
    return std::string(buffer);
}

std::int16_t floatToPcm16(float sample) {
    const float clamped = std::max(-1.0f, std::min(1.0f, sample));
    if (clamped <= -1.0f) {
        return std::numeric_limits<std::int16_t>::min();
    }
    return static_cast<std::int16_t>(std::lrintf(clamped * 32767.0f));
}

int writeAvailablePackets(AVCodecContext* encoderContext, AVFormatContext* formatContext, AVStream* stream, AVPacket* packet, std::string& error) {
    while (true) {
        const int ret = avcodec_receive_packet(encoderContext, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return 0;
        }
        if (ret < 0) {
            error = "avcodec_receive_packet failed: " + ffErrorString(ret);
            return ret;
        }

        av_packet_rescale_ts(packet, encoderContext->time_base, stream->time_base);
        packet->stream_index = stream->index;

        const int writeRet = av_interleaved_write_frame(formatContext, packet);
        av_packet_unref(packet);
        if (writeRet < 0) {
            error = "av_interleaved_write_frame failed: " + ffErrorString(writeRet);
            return writeRet;
        }
    }
}

}  // namespace

bool FfmpegWavAudioExporter::exportPcm16Wav(const AudioBufferF32& audio, const std::string& outputPath, std::string& error) {
    error.clear();

    if (outputPath.empty()) {
        error = "output path is empty";
        return false;
    }
    if (!audio.isConsistent() || audio.empty() || audio.sampleRate <= 0 || audio.channels <= 0) {
        error = "AudioBufferF32 is empty or inconsistent";
        return false;
    }

    std::error_code fsError;
    const std::filesystem::path outputFsPath(outputPath);
    const std::filesystem::path parent = outputFsPath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, fsError);
        if (fsError) {
            error = "failed to create output directory: " + fsError.message();
            return false;
        }
    }

    AVFormatContext* formatContext = nullptr;
    AVCodecContext* encoderContext = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;

    auto cleanup = [&]() {
        if (frame) {
            av_frame_free(&frame);
        }
        if (packet) {
            av_packet_free(&packet);
        }
        if (encoderContext) {
            avcodec_free_context(&encoderContext);
        }
        if (formatContext) {
            if (formatContext->pb) {
                avio_closep(&formatContext->pb);
            }
            avformat_free_context(formatContext);
        }
    };

    int ret = avformat_alloc_output_context2(&formatContext, nullptr, "wav", outputPath.c_str());
    if (ret < 0 || !formatContext) {
        error = "avformat_alloc_output_context2 failed: " + ffErrorString(ret);
        cleanup();
        return false;
    }

    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
    if (!encoder) {
        error = "pcm_s16le encoder not found";
        cleanup();
        return false;
    }

    AVStream* stream = avformat_new_stream(formatContext, nullptr);
    if (!stream) {
        error = "avformat_new_stream failed";
        cleanup();
        return false;
    }

    encoderContext = avcodec_alloc_context3(encoder);
    if (!encoderContext) {
        error = "avcodec_alloc_context3 failed";
        cleanup();
        return false;
    }

    encoderContext->codec_id = AV_CODEC_ID_PCM_S16LE;
    encoderContext->codec_type = AVMEDIA_TYPE_AUDIO;
    encoderContext->sample_fmt = AV_SAMPLE_FMT_S16;
    encoderContext->sample_rate = audio.sampleRate;
    encoderContext->bit_rate = static_cast<std::int64_t>(audio.sampleRate) * audio.channels * 16;
    encoderContext->time_base = AVRational{1, audio.sampleRate};
    av_channel_layout_default(&encoderContext->ch_layout, audio.channels);

    ret = avcodec_open2(encoderContext, encoder, nullptr);
    if (ret < 0) {
        error = "avcodec_open2 failed: " + ffErrorString(ret);
        cleanup();
        return false;
    }

    ret = avcodec_parameters_from_context(stream->codecpar, encoderContext);
    if (ret < 0) {
        error = "avcodec_parameters_from_context failed: " + ffErrorString(ret);
        cleanup();
        return false;
    }
    stream->time_base = encoderContext->time_base;

    if (!(formatContext->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&formatContext->pb, outputPath.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            error = "avio_open failed: " + ffErrorString(ret);
            cleanup();
            return false;
        }
    }

    ret = avformat_write_header(formatContext, nullptr);
    if (ret < 0) {
        error = "avformat_write_header failed: " + ffErrorString(ret);
        cleanup();
        return false;
    }

    packet = av_packet_alloc();
    if (!packet) {
        error = "av_packet_alloc failed";
        cleanup();
        return false;
    }

    const std::int64_t totalFrames = audio.frameCount();
    const int channels = audio.channels;
    const int chunkFrames = encoderContext->frame_size > 0 ? encoderContext->frame_size : 4096;
    std::int64_t frameOffset = 0;

    while (frameOffset < totalFrames) {
        const int nbSamples = static_cast<int>(std::min<std::int64_t>(chunkFrames, totalFrames - frameOffset));

        frame = av_frame_alloc();
        if (!frame) {
            error = "av_frame_alloc failed";
            cleanup();
            return false;
        }

        frame->format = encoderContext->sample_fmt;
        frame->sample_rate = encoderContext->sample_rate;
        frame->nb_samples = nbSamples;
        frame->pts = frameOffset;
        ret = av_channel_layout_copy(&frame->ch_layout, &encoderContext->ch_layout);
        if (ret < 0) {
            error = "av_channel_layout_copy failed: " + ffErrorString(ret);
            cleanup();
            return false;
        }

        ret = av_frame_get_buffer(frame, 0);
        if (ret < 0) {
            error = "av_frame_get_buffer failed: " + ffErrorString(ret);
            cleanup();
            return false;
        }

        auto* dst = reinterpret_cast<std::int16_t*>(frame->data[0]);
        const std::size_t sourceOffset = static_cast<std::size_t>(frameOffset) * static_cast<std::size_t>(channels);
        const std::size_t sampleCount = static_cast<std::size_t>(nbSamples) * static_cast<std::size_t>(channels);
        for (std::size_t i = 0; i < sampleCount; ++i) {
            dst[i] = floatToPcm16(audio.samples[sourceOffset + i]);
        }

        ret = avcodec_send_frame(encoderContext, frame);
        av_frame_free(&frame);
        if (ret < 0) {
            error = "avcodec_send_frame failed: " + ffErrorString(ret);
            cleanup();
            return false;
        }

        ret = writeAvailablePackets(encoderContext, formatContext, stream, packet, error);
        if (ret < 0) {
            cleanup();
            return false;
        }

        frameOffset += nbSamples;
    }

    ret = avcodec_send_frame(encoderContext, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        error = "encoder flush failed: " + ffErrorString(ret);
        cleanup();
        return false;
    }

    ret = writeAvailablePackets(encoderContext, formatContext, stream, packet, error);
    if (ret < 0) {
        cleanup();
        return false;
    }

    ret = av_write_trailer(formatContext);
    if (ret < 0) {
        error = "av_write_trailer failed: " + ffErrorString(ret);
        cleanup();
        return false;
    }

    cleanup();
    return true;
}

}  // namespace AveMediaBridge
