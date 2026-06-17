#include "PacketScan.hpp"

#include "../Core/MediaBridgeError.hpp"
#include "../Ffmpeg/FfmpegDeleters.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

namespace AveMediaBridge::Probe {
namespace {

using AveMediaBridge::ffErrorString;

std::uint32_t readLittleEndianU32(const std::uint8_t* data) {
    if (!data) {
        return 0;
    }
    return static_cast<std::uint32_t>(data[0]) |
        (static_cast<std::uint32_t>(data[1]) << 8) |
        (static_cast<std::uint32_t>(data[2]) << 16) |
        (static_cast<std::uint32_t>(data[3]) << 24);
}

std::int64_t saturatingAddSamples(std::int64_t current, std::uint32_t add) {
    const std::int64_t maxValue = (std::numeric_limits<std::int64_t>::max)();
    const std::int64_t add64 = static_cast<std::int64_t>(add);
    if (current > maxValue - add64) {
        return maxValue;
    }
    return current + add64;
}

std::int64_t saturatingAddInt64(std::int64_t current, std::int64_t add) {
    const std::int64_t maxValue = (std::numeric_limits<std::int64_t>::max)();
    if (add <= 0) {
        return current;
    }
    if (current > maxValue - add) {
        return maxValue;
    }
    return current + add;
}

std::int64_t roundedFramesFromTimeBaseUnits(
    std::int64_t units,
    AVRational timeBase,
    int sampleRate) {
    if (units <= 0 ||
        timeBase.num <= 0 ||
        timeBase.den <= 0 ||
        sampleRate <= 0) {
        return 0;
    }
    const long double frames =
        static_cast<long double>(units) *
        static_cast<long double>(timeBase.num) *
        static_cast<long double>(sampleRate) /
        static_cast<long double>(timeBase.den);
    if (frames <= 0.0L ||
        frames > static_cast<long double>((std::numeric_limits<std::int64_t>::max)())) {
        return 0;
    }
    return static_cast<std::int64_t>(std::llround(frames));
}

std::int64_t roundedFramesFromLongDouble(long double frames) {
    if (frames <= 0.0L ||
        frames > static_cast<long double>((std::numeric_limits<std::int64_t>::max)())) {
        return 0;
    }
    return static_cast<std::int64_t>(std::llround(frames));
}

std::int64_t packetTimestamp(const AVPacket* packet) {
    if (!packet) {
        return AV_NOPTS_VALUE;
    }
    if (packet->pts != AV_NOPTS_VALUE) {
        return packet->pts;
    }
    return packet->dts;
}

}  // namespace

std::int64_t mp3SamplesPerFrameForSampleRate(int sampleRate) {
    if (sampleRate <= 0) {
        return 0;
    }
    return sampleRate < 32000 ? 576 : 1152;
}

std::int64_t wmav2SamplesPerFrameForSampleRate(int sampleRate) {
    if (sampleRate <= 0) {
        return 0;
    }
    if (sampleRate <= 16000) {
        return 512;
    }
    if (sampleRate <= 22050) {
        return 1024;
    }
    return 2048;
}

PacketFrameCountScan scanPacketFrameCountCandidates(
    const std::string& path,
    int audioStreamIndex,
    int sampleRate,
    AVCodecID codecId,
    PacketScanOptions options) {
    PacketFrameCountScan result;
    if (path.empty() || audioStreamIndex < 0 || sampleRate <= 0) {
        return result;
    }

    Ffmpeg::UniqueAVFormatContext scanContext(avformat_alloc_context());
    if (!scanContext) {
        result.warning = "packet frame-count scan failed: avformat_alloc_context";
        return result;
    }

    scanContext->probesize = options.probeSizeBytes;
    scanContext->max_analyze_duration = options.analyzeDurationUs;

    AVDictionary* dictionary = nullptr;
    av_dict_set(&dictionary, "probesize", std::to_string(options.probeSizeBytes).c_str(), 0);
    av_dict_set(&dictionary, "analyzeduration", std::to_string(options.analyzeDurationUs).c_str(), 0);

    AVFormatContext* openContext = scanContext.release();
    int ret = avformat_open_input(&openContext, path.c_str(), nullptr, &dictionary);
    scanContext.reset(openContext);
    av_dict_free(&dictionary);
    if (ret < 0) {
        result.warning = "packet frame-count scan open failed: " + ffErrorString(ret);
        return result;
    }

    ret = avformat_find_stream_info(scanContext.get(), nullptr);
    if (ret < 0) {
        result.warning = "packet frame-count scan stream info failed: " + ffErrorString(ret);
        return result;
    }

    if (audioStreamIndex >= static_cast<int>(scanContext->nb_streams) ||
        !scanContext->streams[audioStreamIndex] ||
        !scanContext->streams[audioStreamIndex]->codecpar ||
        scanContext->streams[audioStreamIndex]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        result.warning = "packet frame-count scan audio stream index mismatch";
        return result;
    }

    const AVStream* audioStream = scanContext->streams[audioStreamIndex];
    const AVRational timeBase = audioStream->time_base;
    Ffmpeg::UniqueAVPacket packet(av_packet_alloc());
    if (!packet) {
        result.warning = "packet frame-count scan failed: av_packet_alloc";
        return result;
    }

    long double packetDurationFrames = 0.0L;
    std::int64_t previousPacketPts = AV_NOPTS_VALUE;
    while ((ret = av_read_frame(scanContext.get(), packet.get())) >= 0) {
        ++result.packetCount;
        if (packet->stream_index == audioStreamIndex) {
            ++result.audioPacketCount;
            if (packet->duration > 0) {
                ++result.packetsWithDuration;
                packetDurationFrames +=
                    static_cast<long double>(packet->duration) *
                    static_cast<long double>(timeBase.num) *
                    static_cast<long double>(sampleRate) /
                    static_cast<long double>(timeBase.den);
                result.lastPacketDuration = packet->duration;
            }

            const std::int64_t packetPts = packetTimestamp(packet.get());
            if (packetPts != AV_NOPTS_VALUE) {
                ++result.packetsWithTimestamp;
                if (previousPacketPts != AV_NOPTS_VALUE && packetPts < previousPacketPts) {
                    result.packetPtsMonotonic = false;
                }
                previousPacketPts = packetPts;
                if (result.firstPacketPts == AV_NOPTS_VALUE || packetPts < result.firstPacketPts) {
                    result.firstPacketPts = packetPts;
                }
                if (result.lastPacketPts == AV_NOPTS_VALUE || packetPts >= result.lastPacketPts) {
                    result.lastPacketPts = packetPts;
                    result.lastPacketEndPts =
                        packet->duration > 0 ? packetPts + packet->duration : packetPts;
                }
            }
        }
        av_packet_unref(packet.get());
    }

    if (ret != AVERROR_EOF) {
        result.warning = "packet frame-count scan read failed: " + ffErrorString(ret);
    }

    result.packetDurationSumFrames = roundedFramesFromLongDouble(packetDurationFrames);
    result.averagePacketDurationFrames =
        result.packetsWithDuration > 0
            ? static_cast<double>(packetDurationFrames) / static_cast<double>(result.packetsWithDuration)
            : 0.0;
    if (result.firstPacketPts != AV_NOPTS_VALUE &&
        result.lastPacketPts != AV_NOPTS_VALUE &&
        result.lastPacketPts > result.firstPacketPts) {
        result.packetPtsSpanWithoutLastDurationFrames = roundedFramesFromTimeBaseUnits(
            result.lastPacketPts - result.firstPacketPts,
            timeBase,
            sampleRate);
    }
    if (result.firstPacketPts != AV_NOPTS_VALUE &&
        result.lastPacketEndPts != AV_NOPTS_VALUE &&
        result.lastPacketEndPts > result.firstPacketPts) {
        result.packetPtsSpanPlusLastDurationFrames = roundedFramesFromTimeBaseUnits(
            result.lastPacketEndPts - result.firstPacketPts,
            timeBase,
            sampleRate);
        result.packetPtsSpanFrames = result.packetPtsSpanPlusLastDurationFrames;
    } else {
        result.packetPtsSpanFrames = result.packetPtsSpanWithoutLastDurationFrames;
    }
    if (codecId == AV_CODEC_ID_AAC && result.audioPacketCount > 0) {
        constexpr std::int64_t kAacSamplesPerPacket = 1024;
        if (result.audioPacketCount <=
            (std::numeric_limits<std::int64_t>::max)() / kAacSamplesPerPacket) {
            result.aacFrameCountCandidateFrames =
                result.audioPacketCount * kAacSamplesPerPacket;
        }
    }
    if (codecId == AV_CODEC_ID_MP3 && result.audioPacketCount > 0) {
        const std::int64_t mp3SamplesPerPacket =
            mp3SamplesPerFrameForSampleRate(sampleRate);
        if (mp3SamplesPerPacket > 0 &&
            result.audioPacketCount <=
                (std::numeric_limits<std::int64_t>::max)() / mp3SamplesPerPacket) {
            result.mp3FrameCountCandidateFrames =
                result.audioPacketCount * mp3SamplesPerPacket;
        }
    }
    if (codecId == AV_CODEC_ID_MP2 && result.audioPacketCount > 0) {
        constexpr std::int64_t kMp2SamplesPerPacket = 1152;
        if (result.audioPacketCount <=
            (std::numeric_limits<std::int64_t>::max)() / kMp2SamplesPerPacket) {
            result.mp2FrameCountCandidateFrames =
                result.audioPacketCount * kMp2SamplesPerPacket;
        }
    }
    if (codecId == AV_CODEC_ID_WMAV2 && result.audioPacketCount > 0) {
        const std::int64_t wmav2SamplesPerPacket =
            wmav2SamplesPerFrameForSampleRate(sampleRate);
        if (wmav2SamplesPerPacket > 0 &&
            result.audioPacketCount <=
                (std::numeric_limits<std::int64_t>::max)() / wmav2SamplesPerPacket) {
            result.wmav2FrameCountCandidateFrames =
                result.audioPacketCount * wmav2SamplesPerPacket;
        }
    }

    return result;
}

GaplessSkipSampleScan scanGaplessSkipSampleSideData(
    const std::string& path,
    int audioStreamIndex,
    PacketScanOptions options) {
    GaplessSkipSampleScan result;
    if (path.empty() || audioStreamIndex < 0) {
        return result;
    }

    Ffmpeg::UniqueAVFormatContext scanContext(avformat_alloc_context());
    if (!scanContext) {
        result.warning = "gapless side-data scan failed: avformat_alloc_context";
        return result;
    }

    scanContext->probesize = options.probeSizeBytes;
    scanContext->max_analyze_duration = options.analyzeDurationUs;

    AVDictionary* dictionary = nullptr;
    av_dict_set(&dictionary, "probesize", std::to_string(options.probeSizeBytes).c_str(), 0);
    av_dict_set(&dictionary, "analyzeduration", std::to_string(options.analyzeDurationUs).c_str(), 0);

    AVFormatContext* openContext = scanContext.release();
    int ret = avformat_open_input(&openContext, path.c_str(), nullptr, &dictionary);
    scanContext.reset(openContext);
    av_dict_free(&dictionary);
    if (ret < 0) {
        result.warning = "gapless side-data scan open failed: " + ffErrorString(ret);
        return result;
    }

    ret = avformat_find_stream_info(scanContext.get(), nullptr);
    if (ret < 0) {
        result.warning = "gapless side-data scan stream info failed: " + ffErrorString(ret);
        return result;
    }

    if (audioStreamIndex >= static_cast<int>(scanContext->nb_streams) ||
        !scanContext->streams[audioStreamIndex] ||
        !scanContext->streams[audioStreamIndex]->codecpar ||
        scanContext->streams[audioStreamIndex]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        result.warning = "gapless side-data scan audio stream index mismatch";
        return result;
    }

    const AVCodecParameters* codecpar = scanContext->streams[audioStreamIndex]->codecpar;
    const std::int64_t streamInitialPadding =
        codecpar && codecpar->initial_padding > 0
            ? static_cast<std::int64_t>(codecpar->initial_padding)
            : 0;
    const std::int64_t streamTrailingPadding =
        codecpar && codecpar->trailing_padding > 0
            ? static_cast<std::int64_t>(codecpar->trailing_padding)
            : 0;

    Ffmpeg::UniqueAVPacket packet(av_packet_alloc());
    if (!packet) {
        result.warning = "gapless side-data scan failed: av_packet_alloc";
        return result;
    }

    while ((ret = av_read_frame(scanContext.get(), packet.get())) >= 0) {
        if (packet->stream_index == audioStreamIndex) {
            ++result.audioPacketsScanned;

            size_t sideDataSize = 0;
            const std::uint8_t* sideData =
                av_packet_get_side_data(packet.get(), AV_PKT_DATA_SKIP_SAMPLES, &sideDataSize);
            if (sideData && sideDataSize >= 10) {
                const std::uint32_t skipSamples = readLittleEndianU32(sideData);
                const std::uint32_t discardPadding = readLittleEndianU32(sideData + 4);
                ++result.sideDataPacketCount;
                result.skipSamplesStart = saturatingAddSamples(result.skipSamplesStart, skipSamples);
                result.skipSamplesEnd = saturatingAddSamples(result.skipSamplesEnd, discardPadding);
            }
        }
        av_packet_unref(packet.get());
    }

    if (ret != AVERROR_EOF) {
        result.warning = "gapless side-data scan read failed: " + ffErrorString(ret);
    }

    if (streamInitialPadding > result.skipSamplesStart) {
        result.skipSamplesStart = streamInitialPadding;
    }
    if (streamTrailingPadding > result.skipSamplesEnd) {
        result.skipSamplesEnd = streamTrailingPadding;
    }
    result.skipSamplesTotal = saturatingAddInt64(result.skipSamplesStart, result.skipSamplesEnd);
    if (result.sideDataPacketCount > 0 &&
        (streamInitialPadding > 0 || streamTrailingPadding > 0)) {
        result.source = "packet_side_data_skip_samples_and_stream_padding";
    } else if (result.sideDataPacketCount > 0) {
        result.source = "packet_side_data_skip_samples";
    } else if (streamInitialPadding > 0 || streamTrailingPadding > 0) {
        result.source = "stream_padding";
    }
    return result;
}

}  // namespace AveMediaBridge::Probe
