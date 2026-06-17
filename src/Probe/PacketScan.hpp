#pragma once

#include "../Ffmpeg/FfmpegHeaders.hpp"

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

std::int64_t mp3SamplesPerFrameForSampleRate(int sampleRate);
std::int64_t wmav2SamplesPerFrameForSampleRate(int sampleRate);

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
