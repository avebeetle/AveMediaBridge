#pragma once

#include "PacketScan.hpp"
#include "../Ffmpeg/FfmpegHeaders.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace AveMediaBridge::Probe {

struct FrameCountAudioInfo {
    AVCodecID codecId = AV_CODEC_ID_NONE;
    std::string codecName;
    int sampleRate = 0;
    int channels = 0;
};

struct FrameCountPolicyState {
    std::string formatName;
    FrameCountAudioInfo selectedAudio;
    std::int64_t decodedSampleFrames = 0;
    std::string decodedSampleFramesKind = "unknown";
    std::string decodedSampleFramesTrust = "unknown";
    std::string decodedSampleFramesSource = "unknown";
    std::int64_t decodedSampleFramesBeforeCorrection = 0;
    std::int64_t packetPtsSpanFrames = 0;
    std::int64_t packetDurationSumFrames = 0;
    bool packetFrameCountCandidateUsed = false;
    std::string frameCountPolicyReason = "unknown";
    std::int64_t decodedSampleFramesBeforeGaplessCorrection = 0;
    std::int64_t skipSamplesStart = 0;
    std::int64_t skipSamplesEnd = 0;
    std::int64_t skipSamplesTotal = 0;
    std::int64_t gaplessCorrectedDecodedSampleFrames = 0;
    bool gaplessCorrectionApplied = false;
    std::string gaplessCorrectionSource = "none";
    std::int64_t gaplessSideDataPacketCount = 0;
    std::int64_t gaplessAudioPacketsScanned = 0;
    std::int64_t estimatedDecodedBytes = 0;
    std::string estimatedDecodedBytesKind = "unknown";
    std::vector<std::string> warnings;
};

bool shouldScanPacketFrameCountCandidates(const FrameCountPolicyState& state);

void recordGaplessSkipSampleScan(
    FrameCountPolicyState& state,
    const GaplessSkipSampleScan& scan);

void applyGaplessSkipSampleCorrection(
    FrameCountPolicyState& state,
    const GaplessSkipSampleScan& scan);

void applyPacketFrameCountPolicies(
    FrameCountPolicyState& state,
    const PacketFrameCountScan& scan);

void finalizeFrameCountTrustPolicy(
    FrameCountPolicyState& state,
    bool selectedCodecKnown,
    bool selectedCodecIsPcm);

}  // namespace AveMediaBridge::Probe
