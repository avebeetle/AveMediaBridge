#include "AveMediaBridge/Import/FfmpegAudioImporter.hpp"

#include "AveMediaBridge/Core/AudioStats.hpp"
#include "../Core/MediaBridgeError.hpp"
#include "../Decode/AudioDecodeHelpers.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <sstream>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace AveMediaBridge {
namespace {

struct SwrState {
    SwrContext* ctx = nullptr;
    AVChannelLayout layout{};
    AVSampleFormat inputFormat = AV_SAMPLE_FMT_NONE;
    int sampleRate = 0;
    int channels = 0;
    bool initialized = false;
};

std::string rationalToString(AVRational value) {
    std::ostringstream out;
    out << value.num << "/" << value.den;
    return out.str();
}

std::string mediaTypeName(AVMediaType type) {
    const char* name = av_get_media_type_string(type);
    return name ? std::string(name) : std::string("unknown");
}

void addWarning(DecodeReport& report, const std::string& warning) {
    ++report.warningsCount;
    if (report.warnings.size() < 16) {
        report.warnings.push_back(warning);
    }
}

void fillSourceInfo(AudioImportResult& result, const AVFormatContext* formatContext, const std::string& path) {
    result.source.inputPath = path;
    result.source.streamCount = static_cast<int>(formatContext->nb_streams);
    if (formatContext->duration != AV_NOPTS_VALUE) {
        result.source.durationSeconds = static_cast<double>(formatContext->duration) / static_cast<double>(AV_TIME_BASE);
    }

    if (formatContext->iformat) {
        result.source.formatName = formatContext->iformat->name ? formatContext->iformat->name : "";
        result.source.formatLongName = formatContext->iformat->long_name ? formatContext->iformat->long_name : "";
    }
}

StreamSummary makeStreamSummary(int index, const AVStream* stream) {
    StreamSummary summary;
    summary.index = index;
    if (!stream || !stream->codecpar) {
        return summary;
    }

    const AVCodecParameters* codecpar = stream->codecpar;
    summary.mediaType = mediaTypeName(codecpar->codec_type);
    summary.codecName = avcodec_get_name(codecpar->codec_id);
    summary.codecId = static_cast<int>(codecpar->codec_id);
    summary.sampleRate = codecpar->sample_rate;
    summary.channels = codecpar->ch_layout.nb_channels;
    summary.bitRate = codecpar->bit_rate;
    summary.timeBase = rationalToString(stream->time_base);
    return summary;
}

void fillProbeDetails(AudioImportResult& result, const AVFormatContext* formatContext) {
    result.probe.streamInfoFound = true;
    result.probe.streams.reserve(formatContext->nb_streams);

    for (unsigned int i = 0; i < formatContext->nb_streams; ++i) {
        const AVStream* stream = formatContext->streams[i];
        StreamSummary summary = makeStreamSummary(static_cast<int>(i), stream);
        if (stream && stream->codecpar && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ++result.probe.audioStreamCount;
        }
        result.probe.streams.push_back(summary);
    }
}

void fillSelectedAudioInfo(AudioImportResult& result, const AVStream* stream, const AVCodec* decoder, const AVCodecContext* decoderContext) {
    if (!stream || !stream->codecpar) {
        return;
    }

    const AVCodecParameters* codecpar = stream->codecpar;
    result.selectedAudio.index = static_cast<int>(stream->index);
    result.selectedAudio.codecName = avcodec_get_name(codecpar->codec_id);
    result.selectedAudio.codecId = static_cast<int>(codecpar->codec_id);
    result.selectedAudio.decoderName = decoder && decoder->name ? decoder->name : "";
    result.selectedAudio.sampleRate = decoderContext ? decoderContext->sample_rate : codecpar->sample_rate;
    result.selectedAudio.channels = decoderContext ? decoderContext->ch_layout.nb_channels : codecpar->ch_layout.nb_channels;
    result.selectedAudio.bitRate = codecpar->bit_rate;
    result.selectedAudio.timeBase = rationalToString(stream->time_base);
}

int ensureSwr(SwrState& swr, const AVFrame* frame, const AVCodecContext* decoder, AudioImportResult& result) {
    if (swr.initialized) {
        return 0;
    }

    const auto inputFormat = static_cast<AVSampleFormat>(frame->format);
    if (inputFormat == AV_SAMPLE_FMT_NONE) {
        result.error = "decoded frame has unknown sample format";
        return AVERROR(EINVAL);
    }

    const int sampleRate = frame->sample_rate > 0 ? frame->sample_rate : decoder->sample_rate;
    if (sampleRate <= 0) {
        result.error = "decoded frame has invalid sample rate";
        return AVERROR(EINVAL);
    }

    AVChannelLayout inputLayout{};
    int ret = Decode::copyDecodedFrameLayout(&inputLayout, frame, decoder);
    if (ret < 0) {
        result.error = "unable to determine decoded channel layout: " + ffErrorString(ret);
        return ret;
    }

    ret = av_channel_layout_copy(&swr.layout, &inputLayout);
    if (ret < 0) {
        av_channel_layout_uninit(&inputLayout);
        result.error = "unable to copy output channel layout: " + ffErrorString(ret);
        return ret;
    }

    swr.sampleRate = sampleRate;
    swr.channels = swr.layout.nb_channels;
    swr.inputFormat = inputFormat;

    ret = swr_alloc_set_opts2(
        &swr.ctx,
        &swr.layout,
        AV_SAMPLE_FMT_FLT,
        swr.sampleRate,
        &inputLayout,
        inputFormat,
        sampleRate,
        0,
        nullptr);
    av_channel_layout_uninit(&inputLayout);

    if (ret < 0) {
        result.error = "swr_alloc_set_opts2 failed: " + ffErrorString(ret);
        return ret;
    }

    ret = swr_init(swr.ctx);
    if (ret < 0) {
        result.error = "swr_init failed: " + ffErrorString(ret);
        return ret;
    }

    swr.initialized = true;
    result.decode.swrInitialized = true;
    result.decode.outputSampleRate = swr.sampleRate;
    result.decode.outputChannels = swr.channels;
    result.audio.sampleRate = swr.sampleRate;
    result.audio.channels = swr.channels;

    const char* sampleFormatName = av_get_sample_fmt_name(inputFormat);
    result.selectedAudio.decoderSampleFormat = sampleFormatName ? sampleFormatName : "";
    return 0;
}

int appendConvertedFrame(SwrState& swr, const AVFrame* frame, const AVCodecContext* decoder, AudioImportResult& result) {
    int ret = ensureSwr(swr, frame, decoder, result);
    if (ret < 0) {
        return ret;
    }

    const int outCapacity = swr_get_out_samples(swr.ctx, frame->nb_samples);
    if (outCapacity < 0) {
        result.error = "swr_get_out_samples failed: " + ffErrorString(outCapacity);
        return outCapacity;
    }
    if (outCapacity == 0) {
        return 0;
    }

    const std::size_t oldSize = result.audio.samples.size();
    const std::size_t reserveCount = static_cast<std::size_t>(outCapacity) * static_cast<std::size_t>(swr.channels);
    result.audio.samples.resize(oldSize + reserveCount);

    uint8_t* outData[] = { reinterpret_cast<uint8_t*>(result.audio.samples.data() + oldSize) };
    const uint8_t* const* inData = const_cast<const uint8_t* const*>(frame->extended_data);

    ret = swr_convert(swr.ctx, outData, outCapacity, inData, frame->nb_samples);
    if (ret < 0) {
        result.audio.samples.resize(oldSize);
        result.error = "swr_convert failed: " + ffErrorString(ret);
        return ret;
    }

    const std::size_t writtenCount = static_cast<std::size_t>(ret) * static_cast<std::size_t>(swr.channels);
    result.audio.samples.resize(oldSize + writtenCount);
    result.decode.outputSamplesPerChannel += ret;
    result.decode.outputInterleavedFloatSamples += static_cast<std::int64_t>(writtenCount);
    return 0;
}

int flushSwr(SwrState& swr, AudioImportResult& result) {
    if (!swr.initialized) {
        return 0;
    }

    while (true) {
        const std::int64_t delay = swr_get_delay(swr.ctx, swr.sampleRate);
        if (delay <= 0) {
            break;
        }

        const int outCapacity = delay > static_cast<std::int64_t>(INT_MAX) ? INT_MAX : static_cast<int>(delay);
        const std::size_t oldSize = result.audio.samples.size();
        const std::size_t reserveCount = static_cast<std::size_t>(outCapacity) * static_cast<std::size_t>(swr.channels);
        result.audio.samples.resize(oldSize + reserveCount);

        uint8_t* outData[] = { reinterpret_cast<uint8_t*>(result.audio.samples.data() + oldSize) };
        const int ret = swr_convert(swr.ctx, outData, outCapacity, nullptr, 0);
        if (ret < 0) {
            result.audio.samples.resize(oldSize);
            result.error = "swr_convert flush failed: " + ffErrorString(ret);
            return ret;
        }
        if (ret == 0) {
            result.audio.samples.resize(oldSize);
            break;
        }

        const std::size_t writtenCount = static_cast<std::size_t>(ret) * static_cast<std::size_t>(swr.channels);
        result.audio.samples.resize(oldSize + writtenCount);
        result.decode.outputSamplesPerChannel += ret;
        result.decode.outputInterleavedFloatSamples += static_cast<std::int64_t>(writtenCount);
    }

    return 0;
}

int receiveFrames(AVCodecContext* decoder, AVFrame* frame, SwrState& swr, AudioImportResult& result) {
    while (true) {
        const int ret = avcodec_receive_frame(decoder, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return 0;
        }
        if (ret < 0) {
            addWarning(result.decode, "avcodec_receive_frame skipped error: " + ffErrorString(ret));
            return 0;
        }

        ++result.decode.decodedFrames;
        const int convertRet = appendConvertedFrame(swr, frame, decoder, result);
        av_frame_unref(frame);
        if (convertRet < 0) {
            return convertRet;
        }
    }
}

int sendPacketAndReceive(AVCodecContext* decoder, AVPacket* packet, AVFrame* frame, SwrState& swr, AudioImportResult& result) {
    int ret = avcodec_send_packet(decoder, packet);
    if (ret == AVERROR(EAGAIN)) {
        ret = receiveFrames(decoder, frame, swr, result);
        if (ret < 0) {
            return ret;
        }
        ret = avcodec_send_packet(decoder, packet);
    }

    if (ret < 0) {
        ++result.decode.invalidPacketsSkipped;
        addWarning(result.decode, "avcodec_send_packet skipped packet: " + ffErrorString(ret));
        return 0;
    }

    return receiveFrames(decoder, frame, swr, result);
}

}  // namespace

AudioImportResult FfmpegAudioImporter::importFile(const std::string& path) {
    AudioImportResult result;
    result.source.inputPath = path;

    AVFormatContext* formatContext = nullptr;
    AVCodecContext* decoderContext = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    SwrState swr;

    auto cleanup = [&]() {
        if (packet) {
            av_packet_free(&packet);
        }
        if (frame) {
            av_frame_free(&frame);
        }
        if (swr.ctx) {
            swr_free(&swr.ctx);
        }
        av_channel_layout_uninit(&swr.layout);
        if (decoderContext) {
            avcodec_free_context(&decoderContext);
        }
        if (formatContext) {
            avformat_close_input(&formatContext);
        }
    };

    int ret = avformat_open_input(&formatContext, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        result.error = "avformat_open_input failed: " + ffErrorString(ret);
        cleanup();
        return result;
    }

    ret = avformat_find_stream_info(formatContext, nullptr);
    if (ret < 0) {
        result.error = "avformat_find_stream_info failed: " + ffErrorString(ret);
        cleanup();
        return result;
    }

    fillSourceInfo(result, formatContext, path);
    fillProbeDetails(result, formatContext);

    const AVCodec* decoder = nullptr;
    const int audioStreamIndex = av_find_best_stream(formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (audioStreamIndex < 0) {
        result.error = "no audio stream found: " + ffErrorString(audioStreamIndex);
        cleanup();
        return result;
    }

    AVStream* audioStream = formatContext->streams[audioStreamIndex];
    AVCodecParameters* codecpar = audioStream->codecpar;
    if (!decoder) {
        decoder = avcodec_find_decoder(codecpar->codec_id);
    }
    if (!decoder) {
        result.selectedAudio.index = audioStreamIndex;
        result.selectedAudio.codecName = avcodec_get_name(codecpar->codec_id);
        result.error = "decoder not found for codec " + result.selectedAudio.codecName;
        cleanup();
        return result;
    }

    decoderContext = avcodec_alloc_context3(decoder);
    if (!decoderContext) {
        result.error = "avcodec_alloc_context3 failed";
        cleanup();
        return result;
    }

    ret = avcodec_parameters_to_context(decoderContext, codecpar);
    if (ret < 0) {
        result.error = "avcodec_parameters_to_context failed: " + ffErrorString(ret);
        cleanup();
        return result;
    }

    if (decoderContext->ch_layout.nb_channels <= 0 && codecpar->ch_layout.nb_channels > 0) {
        av_channel_layout_copy(&decoderContext->ch_layout, &codecpar->ch_layout);
    }

    ret = avcodec_open2(decoderContext, decoder, nullptr);
    if (ret < 0) {
        result.error = "avcodec_open2 failed: " + ffErrorString(ret);
        cleanup();
        return result;
    }
    result.decode.decoderOpened = true;
    fillSelectedAudioInfo(result, audioStream, decoder, decoderContext);

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!packet || !frame) {
        result.error = "unable to allocate AVPacket/AVFrame";
        cleanup();
        return result;
    }

    while ((ret = av_read_frame(formatContext, packet)) >= 0) {
        ++result.decode.packetsRead;
        if (packet->stream_index == audioStreamIndex) {
            ++result.decode.audioPackets;
            const int decodeRet = sendPacketAndReceive(decoderContext, packet, frame, swr, result);
            av_packet_unref(packet);
            if (decodeRet < 0) {
                cleanup();
                return result;
            }
        } else {
            av_packet_unref(packet);
        }
    }

    if (ret != AVERROR_EOF) {
        result.error = "av_read_frame failed: " + ffErrorString(ret);
        cleanup();
        return result;
    }

    ret = avcodec_send_packet(decoderContext, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        result.error = "decoder flush send failed: " + ffErrorString(ret);
        cleanup();
        return result;
    }

    ret = receiveFrames(decoderContext, frame, swr, result);
    if (ret < 0) {
        cleanup();
        return result;
    }

    ret = flushSwr(swr, result);
    if (ret < 0) {
        cleanup();
        return result;
    }

    if (result.decode.decodedFrames <= 0 || result.audio.samples.empty()) {
        result.error = "no decoded audio samples were produced";
        cleanup();
        return result;
    }

    if (!result.audio.isConsistent()) {
        result.error = "AudioBufferF32 consistency check failed";
        cleanup();
        return result;
    }

    result.stats = computeAudioStats(result.audio);
    result.ok = true;
    cleanup();
    return result;
}

}  // namespace AveMediaBridge
