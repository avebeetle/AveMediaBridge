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
    std::string mp3HeaderPresentationStatus = "not_eligible";
    std::string mp3HeaderPresentationReason = "not_eligible";
    std::string mp3HeaderType = "none";
    std::string mp3HeaderEncoderProfile;
    std::uint64_t mp3HeaderBudgetBytes = 0;
    std::uint64_t mp3HeaderActualReadBytes = 0;
    std::uint64_t mp3HeaderUniqueBytesRead = 0;
    std::uint64_t mp3HeaderMaximumBudgetOverrunBytes = 0;
    std::uint64_t mp3HeaderReadCalls = 0;
    std::uint64_t mp3HeaderSeekCalls = 0;
    std::uint64_t mp3HeaderMaximumOffsetReached = 0;
    std::uint64_t mp3HeaderPhysicalFrameCount = 0;
    std::uint64_t mp3HeaderSamplesPerFrame = 0;
    std::uint64_t mp3HeaderPhysicalSampleTotal = 0;
    std::uint64_t mp3HeaderInitialPresentationSkip = 0;
    std::uint64_t mp3HeaderTerminalPresentationPadding = 0;
    std::uint64_t mp3HeaderPresentationFrames = 0;
    bool mp3HeaderFullScanSkipped = false;
    std::string nutBoundedTailStatus = "not_eligible";
    std::string nutBoundedTailReason = "not_eligible";
    std::uint64_t nutBoundedTailBudgetBytes = 0;
    std::uint64_t nutBoundedTailActualReadBytes = 0;
    std::uint64_t nutBoundedTailMaximumBudgetOverrunBytes = 0;
    std::uint64_t nutBoundedTailPacketsObserved = 0;
    bool nutBoundedTailReachedEof = false;
    bool oggOpusSequentialEligible = false;
    bool oggOpusSequentialEntered = false;
    std::string oggOpusSequentialStatus = "not_eligible";
    std::string oggOpusSequentialReason = "not_eligible";
    std::uint32_t oggOpusSequentialSelectedSerial = 0;
    std::uint64_t oggOpusSequentialPreSkip = 0;
    std::uint64_t oggOpusSequentialPhysicalPacketFrames = 0;
    std::uint64_t oggOpusSequentialLastPacketDuration = 0;
    std::uint64_t oggOpusSequentialEosGranule = 0;
    std::uint64_t oggOpusSequentialTerminalDiscard = 0;
    std::uint64_t oggOpusSequentialPresentationFrames = 0;
    std::uint64_t oggOpusSequentialFileSizeBytes = 0;
    std::uint64_t oggOpusSequentialFileIndex = 0;
    std::uint64_t oggOpusSequentialLastWriteTime100ns = 0;
    std::uint32_t oggOpusSequentialVolumeSerialNumber = 0;
    std::uint64_t oggOpusSequentialBytesReturned = 0;
    std::uint64_t oggOpusSequentialUniqueBytes = 0;
    std::uint64_t oggOpusSequentialDuplicateBytes = 0;
    std::uint64_t oggOpusSequentialReadCalls = 0;
    std::uint64_t oggOpusSequentialSeekCallsAfterOpen = 0;
    std::uint64_t oggOpusSequentialScanDurationUs = 0;
    std::uint64_t oggOpusSequentialPagesParsed = 0;
    std::uint64_t oggOpusSequentialSelectedPages = 0;
    std::uint64_t oggOpusSequentialSelectedAudioPackets = 0;
    std::uint64_t oggOpusSequentialMaximumPacketBytes = 0;
    std::uint64_t oggOpusSequentialMaximumWorkingBufferBytes = 0;
    bool oggOpusSequentialReachedEof = false;
    bool oggOpusSequentialAllPageCrcValid = true;
    bool oggOpusSequentialSelectedSequenceContinuous = true;
    bool oggOpusSequentialPacketContinuityValid = true;
    bool oggOpusSequentialFinalGranuleInPacketInterval = false;
    bool oggOpusSequentialGenericScanEntered = false;
    bool oggOpusSequentialGenericScanSkipped = false;
    bool oggOpusSequentialPossibleDoublePass = false;
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
