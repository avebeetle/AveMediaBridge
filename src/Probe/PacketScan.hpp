#pragma once

#include "../Ffmpeg/FfmpegHeaders.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace AveMediaBridge::Probe {

struct PacketScanOptions {
    std::int64_t probeSizeBytes = 0;
    std::int64_t analyzeDurationUs = 0;
};

struct GaplessSkipSampleScan {
    std::int64_t skipSamplesStart = 0;
    std::int64_t skipSamplesEnd = 0;
    std::int64_t skipSamplesTotal = 0;
    std::int64_t sideDataPacketCount = 0;
    std::int64_t audioPacketsScanned = 0;
    std::string source = "none";
    std::string warning;
};

struct PacketFrameCountScan {
    std::int64_t packetCount = 0;
    std::int64_t audioPacketCount = 0;
    std::int64_t packetsWithDuration = 0;
    std::int64_t packetsWithTimestamp = 0;
    std::int64_t firstPacketPts = AV_NOPTS_VALUE;
    std::int64_t lastPacketPts = AV_NOPTS_VALUE;
    std::int64_t lastPacketEndPts = AV_NOPTS_VALUE;
    std::int64_t lastPacketDuration = 0;
    std::int64_t packetPtsSpanFrames = 0;
    std::int64_t packetPtsSpanWithoutLastDurationFrames = 0;
    std::int64_t packetPtsSpanPlusLastDurationFrames = 0;
    std::int64_t packetDurationSumFrames = 0;
    std::int64_t aacFrameCountCandidateFrames = 0;
    std::int64_t mp3FrameCountCandidateFrames = 0;
    std::int64_t mp2FrameCountCandidateFrames = 0;
    std::int64_t wmav2FrameCountCandidateFrames = 0;
    double averagePacketDurationFrames = 0.0;
    bool packetPtsMonotonic = true;
    std::string warning;
};

struct AudioPresentationEvidenceScan {
    PacketFrameCountScan packetTiming;
    GaplessSkipSampleScan gapless;
};

struct PacketFrameCountObservation {
    bool selectedAudio = false;
    std::int64_t pts = AV_NOPTS_VALUE;
    std::int64_t dts = AV_NOPTS_VALUE;
    std::int64_t duration = 0;
};

class PacketFrameCountAccumulator {
public:
    PacketFrameCountAccumulator(int sampleRate, AVCodecID codecId, AVRational timeBase);

    void observe(const PacketFrameCountObservation& packet);
    PacketFrameCountScan finalize(std::string warning = {}) const;

private:
    int sampleRate_ = 0;
    AVCodecID codecId_ = AV_CODEC_ID_NONE;
    AVRational timeBase_{0, 1};
    PacketFrameCountScan result_;
    long double packetDurationFrames_ = 0.0L;
    std::int64_t previousPacketPts_ = AV_NOPTS_VALUE;
};

struct GaplessSkipObservation {
    bool selectedAudio = false;
    const std::uint8_t* sideData = nullptr;
    std::size_t sideDataSize = 0;
};

class GaplessSkipAccumulator {
public:
    GaplessSkipAccumulator(
        std::int64_t streamInitialPadding,
        std::int64_t streamTrailingPadding);

    void observe(const GaplessSkipObservation& packet);
    GaplessSkipSampleScan finalize(std::string warning = {}) const;

private:
    std::int64_t streamInitialPadding_ = 0;
    std::int64_t streamTrailingPadding_ = 0;
    GaplessSkipSampleScan result_;
};

std::int64_t mp3SamplesPerFrameForSampleRate(int sampleRate);
std::int64_t wmav2SamplesPerFrameForSampleRate(int sampleRate);
std::int64_t saturatingAddSkipSamples(std::int64_t current, std::uint32_t add);

AudioPresentationEvidenceScan scanAudioPresentationEvidence(
    const std::string& path,
    int audioStreamIndex,
    int sampleRate,
    AVCodecID codecId,
    PacketScanOptions options);

GaplessSkipSampleScan scanGaplessSkipSampleSideData(
    const std::string& path,
    int audioStreamIndex,
    PacketScanOptions options);

PacketFrameCountScan scanPacketFrameCountCandidates(
    const std::string& path,
    int audioStreamIndex,
    int sampleRate,
    AVCodecID codecId,
    PacketScanOptions options);

}  // namespace AveMediaBridge::Probe
