#pragma once

#include "AveMediaBridge/Core/MediaInfo.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace AveMediaBridge::Probe {

struct FastProbeJsonDocument {
    std::string sourcePath;
    std::string formatName;
    std::string formatLongName;
    std::string containerFormat;
    int streamCount = 0;
    int audioStreamCount = 0;
    bool hasAudio = false;
    int bestAudioStreamIndex = -1;
    SelectedAudioStreamInfo selectedAudio;
    std::string channelLayout;
    double durationSec = 0.0;
    std::string durationKind = "unknown";
    std::string durationEstimationMethod = "unknown";
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
    std::string nutBoundedTailStatus = "not_eligible";
    std::string nutBoundedTailReason = "not_eligible";
    std::uint64_t nutBoundedTailBudgetBytes = 0;
    std::uint64_t nutBoundedTailActualReadBytes = 0;
    std::uint64_t nutBoundedTailMaximumBudgetOverrunBytes = 0;
    std::uint64_t nutBoundedTailPacketsObserved = 0;
    bool nutBoundedTailReachedEof = false;
    int probeScore = -1;
    std::vector<StreamSummary> streams;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

bool writeProbeJson(
    const std::filesystem::path& outputPath,
    const FastProbeJsonDocument& document,
    std::string& error);

}  // namespace AveMediaBridge::Probe
