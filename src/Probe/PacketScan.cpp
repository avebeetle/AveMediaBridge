#include "PacketScan.hpp"

#include "../Core/MediaBridgeError.hpp"
#include "../Ffmpeg/FfmpegDeleters.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

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

std::int64_t packetTimestamp(const PacketFrameCountObservation& packet) {
    if (packet.pts != AV_NOPTS_VALUE) {
        return packet.pts;
    }
    return packet.dts;
}

void setScanWarnings(
    AudioPresentationEvidenceScan& result,
    bool collectPacketTiming,
    bool collectGapless,
    std::string packetWarning,
    std::string gaplessWarning) {
    // Keep component warnings separate: either failed evidence path must
    // reject presentation-budget authority.
    if (collectPacketTiming) {
        result.packetTiming.warning = std::move(packetWarning);
    }
    if (collectGapless) {
        result.gapless.warning = std::move(gaplessWarning);
    }
}

AudioPresentationEvidenceScan scanAudioPresentationEvidenceImpl(
    const std::string& path,
    int audioStreamIndex,
    int sampleRate,
    AVCodecID codecId,
    PacketScanOptions options,
    bool collectPacketTiming,
    bool collectGapless) {
    AudioPresentationEvidenceScan result;
    if (path.empty() || audioStreamIndex < 0) {
        return result;
    }

    collectPacketTiming = collectPacketTiming && sampleRate > 0;
    if (!collectPacketTiming && !collectGapless) {
        return result;
    }

    Ffmpeg::UniqueAVFormatContext scanContext(avformat_alloc_context());
    if (!scanContext) {
        setScanWarnings(
            result,
            collectPacketTiming,
            collectGapless,
            "packet frame-count scan failed: avformat_alloc_context",
            "gapless side-data scan failed: avformat_alloc_context");
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
        const std::string error = ffErrorString(ret);
        setScanWarnings(
            result,
            collectPacketTiming,
            collectGapless,
            "packet frame-count scan open failed: " + error,
            "gapless side-data scan open failed: " + error);
        return result;
    }

    ret = avformat_find_stream_info(scanContext.get(), nullptr);
    if (ret < 0) {
        const std::string error = ffErrorString(ret);
        setScanWarnings(
            result,
            collectPacketTiming,
            collectGapless,
            "packet frame-count scan stream info failed: " + error,
            "gapless side-data scan stream info failed: " + error);
        return result;
    }

    if (audioStreamIndex >= static_cast<int>(scanContext->nb_streams) ||
        !scanContext->streams[audioStreamIndex] ||
        !scanContext->streams[audioStreamIndex]->codecpar ||
        scanContext->streams[audioStreamIndex]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        setScanWarnings(
            result,
            collectPacketTiming,
            collectGapless,
            "packet frame-count scan audio stream index mismatch",
            "gapless side-data scan audio stream index mismatch");
        return result;
    }

    const AVStream* audioStream = scanContext->streams[audioStreamIndex];
    const AVCodecParameters* codecpar = audioStream->codecpar;
    const std::int64_t streamInitialPadding =
        codecpar->initial_padding > 0
            ? static_cast<std::int64_t>(codecpar->initial_padding)
            : 0;
    const std::int64_t streamTrailingPadding =
        codecpar->trailing_padding > 0
            ? static_cast<std::int64_t>(codecpar->trailing_padding)
            : 0;

    std::optional<PacketFrameCountAccumulator> packetTiming;
    if (collectPacketTiming) {
        packetTiming.emplace(sampleRate, codecId, audioStream->time_base);
    }
    std::optional<GaplessSkipAccumulator> gapless;
    if (collectGapless) {
        gapless.emplace(streamInitialPadding, streamTrailingPadding);
    }

    Ffmpeg::UniqueAVPacket packet(av_packet_alloc());
    if (!packet) {
        setScanWarnings(
            result,
            collectPacketTiming,
            collectGapless,
            "packet frame-count scan failed: av_packet_alloc",
            "gapless side-data scan failed: av_packet_alloc");
        return result;
    }

    // Packet timing and gapless metadata remain independent evidence
    // models, but share one physical demux traversal.
    while ((ret = av_read_frame(scanContext.get(), packet.get())) >= 0) {
        const bool selectedAudio = packet->stream_index == audioStreamIndex;
        if (packetTiming) {
            packetTiming->observe(PacketFrameCountObservation{
                selectedAudio,
                packet->pts,
                packet->dts,
                packet->duration
            });
        }
        if (gapless) {
            std::size_t sideDataSize = 0;
            const std::uint8_t* sideData = selectedAudio
                ? av_packet_get_side_data(
                    packet.get(),
                    AV_PKT_DATA_SKIP_SAMPLES,
                    &sideDataSize)
                : nullptr;
            gapless->observe(GaplessSkipObservation{
                selectedAudio,
                sideData,
                sideDataSize
            });
        }
        av_packet_unref(packet.get());
    }

    const std::string readError = ret != AVERROR_EOF ? ffErrorString(ret) : std::string{};
    if (packetTiming) {
        result.packetTiming = packetTiming->finalize(
            readError.empty() ? std::string{} : "packet frame-count scan read failed: " + readError);
    }
    if (gapless) {
        result.gapless = gapless->finalize(
            readError.empty() ? std::string{} : "gapless side-data scan read failed: " + readError);
    }
    return result;
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

std::int64_t saturatingAddSkipSamples(std::int64_t current, std::uint32_t add) {
    const std::int64_t maxValue = (std::numeric_limits<std::int64_t>::max)();
    const std::int64_t add64 = static_cast<std::int64_t>(add);
    if (current > maxValue - add64) {
        return maxValue;
    }
    return current + add64;
}

PacketFrameCountAccumulator::PacketFrameCountAccumulator(
    int sampleRate,
    AVCodecID codecId,
    AVRational timeBase)
    : sampleRate_(sampleRate), codecId_(codecId), timeBase_(timeBase) {}

void PacketFrameCountAccumulator::observe(const PacketFrameCountObservation& packet) {
    ++result_.packetCount;
    if (!packet.selectedAudio) {
        return;
    }

    ++result_.audioPacketCount;
    if (packet.duration > 0) {
        ++result_.packetsWithDuration;
        packetDurationFrames_ +=
            static_cast<long double>(packet.duration) *
            static_cast<long double>(timeBase_.num) *
            static_cast<long double>(sampleRate_) /
            static_cast<long double>(timeBase_.den);
        result_.lastPacketDuration = packet.duration;
    }

    const std::int64_t currentPacketPts = packetTimestamp(packet);
    if (currentPacketPts == AV_NOPTS_VALUE) {
        return;
    }

    ++result_.packetsWithTimestamp;
    if (previousPacketPts_ != AV_NOPTS_VALUE && currentPacketPts < previousPacketPts_) {
        result_.packetPtsMonotonic = false;
    }
    previousPacketPts_ = currentPacketPts;
    if (result_.firstPacketPts == AV_NOPTS_VALUE || currentPacketPts < result_.firstPacketPts) {
        result_.firstPacketPts = currentPacketPts;
    }
    if (result_.lastPacketPts == AV_NOPTS_VALUE || currentPacketPts >= result_.lastPacketPts) {
        result_.lastPacketPts = currentPacketPts;
        result_.lastPacketEndPts =
            packet.duration > 0 ? currentPacketPts + packet.duration : currentPacketPts;
    }
}

PacketFrameCountScan PacketFrameCountAccumulator::finalize(std::string warning) const {
    PacketFrameCountScan result = result_;
    result.warning = std::move(warning);
    result.packetDurationSumFrames = roundedFramesFromLongDouble(packetDurationFrames_);
    result.averagePacketDurationFrames =
        result.packetsWithDuration > 0
            ? static_cast<double>(packetDurationFrames_) /
                static_cast<double>(result.packetsWithDuration)
            : 0.0;
    if (result.firstPacketPts != AV_NOPTS_VALUE &&
        result.lastPacketPts != AV_NOPTS_VALUE &&
        result.lastPacketPts > result.firstPacketPts) {
        result.packetPtsSpanWithoutLastDurationFrames = roundedFramesFromTimeBaseUnits(
            result.lastPacketPts - result.firstPacketPts,
            timeBase_,
            sampleRate_);
    }
    if (result.firstPacketPts != AV_NOPTS_VALUE &&
        result.lastPacketEndPts != AV_NOPTS_VALUE &&
        result.lastPacketEndPts > result.firstPacketPts) {
        result.packetPtsSpanPlusLastDurationFrames = roundedFramesFromTimeBaseUnits(
            result.lastPacketEndPts - result.firstPacketPts,
            timeBase_,
            sampleRate_);
        result.packetPtsSpanFrames = result.packetPtsSpanPlusLastDurationFrames;
    } else {
        result.packetPtsSpanFrames = result.packetPtsSpanWithoutLastDurationFrames;
    }
    if (codecId_ == AV_CODEC_ID_AAC && result.audioPacketCount > 0) {
        constexpr std::int64_t kAacSamplesPerPacket = 1024;
        if (result.audioPacketCount <=
            (std::numeric_limits<std::int64_t>::max)() / kAacSamplesPerPacket) {
            result.aacFrameCountCandidateFrames =
                result.audioPacketCount * kAacSamplesPerPacket;
        }
    }
    if (codecId_ == AV_CODEC_ID_MP3 && result.audioPacketCount > 0) {
        const std::int64_t samplesPerPacket = mp3SamplesPerFrameForSampleRate(sampleRate_);
        if (samplesPerPacket > 0 &&
            result.audioPacketCount <=
                (std::numeric_limits<std::int64_t>::max)() / samplesPerPacket) {
            result.mp3FrameCountCandidateFrames = result.audioPacketCount * samplesPerPacket;
        }
    }
    if (codecId_ == AV_CODEC_ID_MP2 && result.audioPacketCount > 0) {
        constexpr std::int64_t kMp2SamplesPerPacket = 1152;
        if (result.audioPacketCount <=
            (std::numeric_limits<std::int64_t>::max)() / kMp2SamplesPerPacket) {
            result.mp2FrameCountCandidateFrames =
                result.audioPacketCount * kMp2SamplesPerPacket;
        }
    }
    if (codecId_ == AV_CODEC_ID_WMAV2 && result.audioPacketCount > 0) {
        const std::int64_t samplesPerPacket = wmav2SamplesPerFrameForSampleRate(sampleRate_);
        if (samplesPerPacket > 0 &&
            result.audioPacketCount <=
                (std::numeric_limits<std::int64_t>::max)() / samplesPerPacket) {
            result.wmav2FrameCountCandidateFrames =
                result.audioPacketCount * samplesPerPacket;
        }
    }
    return result;
}

GaplessSkipAccumulator::GaplessSkipAccumulator(
    std::int64_t streamInitialPadding,
    std::int64_t streamTrailingPadding)
    : streamInitialPadding_((std::max)(std::int64_t{0}, streamInitialPadding)),
      streamTrailingPadding_((std::max)(std::int64_t{0}, streamTrailingPadding)) {}

void GaplessSkipAccumulator::observe(const GaplessSkipObservation& packet) {
    if (!packet.selectedAudio) {
        return;
    }

    ++result_.audioPacketsScanned;
    if (!packet.sideData || packet.sideDataSize < 10) {
        return;
    }

    const std::uint32_t skipSamples = readLittleEndianU32(packet.sideData);
    const std::uint32_t discardPadding = readLittleEndianU32(packet.sideData + 4);
    ++result_.sideDataPacketCount;
    result_.skipSamplesStart = saturatingAddSkipSamples(result_.skipSamplesStart, skipSamples);
    result_.skipSamplesEnd = saturatingAddSkipSamples(result_.skipSamplesEnd, discardPadding);
}

GaplessSkipSampleScan GaplessSkipAccumulator::finalize(std::string warning) const {
    GaplessSkipSampleScan result = result_;
    result.warning = std::move(warning);
    result.skipSamplesStart = (std::max)(result.skipSamplesStart, streamInitialPadding_);
    result.skipSamplesEnd = (std::max)(result.skipSamplesEnd, streamTrailingPadding_);
    result.skipSamplesTotal = saturatingAddInt64(result.skipSamplesStart, result.skipSamplesEnd);
    if (result.sideDataPacketCount > 0 &&
        (streamInitialPadding_ > 0 || streamTrailingPadding_ > 0)) {
        result.source = "packet_side_data_skip_samples_and_stream_padding";
    } else if (result.sideDataPacketCount > 0) {
        result.source = "packet_side_data_skip_samples";
    } else if (streamInitialPadding_ > 0 || streamTrailingPadding_ > 0) {
        result.source = "stream_padding";
    }
    return result;
}

AudioPresentationEvidenceScan scanAudioPresentationEvidence(
    const std::string& path,
    int audioStreamIndex,
    int sampleRate,
    AVCodecID codecId,
    PacketScanOptions options) {
    return scanAudioPresentationEvidenceImpl(
        path,
        audioStreamIndex,
        sampleRate,
        codecId,
        options,
        true,
        true);
}

PacketFrameCountScan scanPacketFrameCountCandidates(
    const std::string& path,
    int audioStreamIndex,
    int sampleRate,
    AVCodecID codecId,
    PacketScanOptions options) {
    return scanAudioPresentationEvidenceImpl(
        path,
        audioStreamIndex,
        sampleRate,
        codecId,
        options,
        true,
        false).packetTiming;
}

GaplessSkipSampleScan scanGaplessSkipSampleSideData(
    const std::string& path,
    int audioStreamIndex,
    PacketScanOptions options) {
    return scanAudioPresentationEvidenceImpl(
        path,
        audioStreamIndex,
        0,
        AV_CODEC_ID_NONE,
        options,
        false,
        true).gapless;
}

}  // namespace AveMediaBridge::Probe
