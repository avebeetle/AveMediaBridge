#pragma once

#include "../Ffmpeg/FfmpegHeaders.hpp"

#include <cstdint>
#include <string>

namespace AveMediaBridge::Probe {

constexpr std::uint64_t kNutBoundedTailReadBudgetBytes = 256ULL * 1024ULL;

enum class NutBoundedTailStatus {
    Exact,
    Unavailable,
    IoBudgetExceeded,
    Conflict,
    InvalidMedia
};

struct PcmPacketLayout {
    bool eligible = false;
    int sampleRate = 0;
    int channels = 0;
    int bytesPerSample = 0;
    std::int64_t bytesPerFrame = 0;
    std::string rejectionReason = "unsupported_codec";
};

struct NutBoundedTailTestHooks {
    bool forceNonSeekable = false;
    bool forceNoIndex = false;
    bool forceSeekFailure = false;
    bool forceMissingDuration = false;
    bool forceMisalignedPayload = false;
    bool suppressSelectedPackets = false;
    std::uint64_t readErrorAfterBytes = 0;
    std::uint64_t syntheticEofAfterBytes = 0;
    bool overrideSeekTarget = false;
    std::int64_t seekTarget = 0;
};

struct NutBoundedTailProbeOptions {
    std::uint64_t hardReadBudgetBytes = kNutBoundedTailReadBudgetBytes;
    const NutBoundedTailTestHooks* testHooks = nullptr;
};

struct NutBoundedTailProbeResult {
    NutBoundedTailStatus status = NutBoundedTailStatus::Unavailable;
    std::uint64_t presentationFrames = 0;
    std::uint64_t hardReadBudgetBytes = 0;
    std::uint64_t totalActualReadBytes = 0;
    std::uint64_t maximumBudgetOverrunBytes = 0;
    std::uint64_t targetPacketsObserved = 0;
    int selectedStreamIndex = -1;
    int sampleRate = 0;
    bool timestampSeekSucceeded = false;
    bool reachedPhysicalEof = false;
    bool payloadAligned = false;
    bool durationPayloadAgree = false;
    std::string reason = "not_eligible";

    bool exact() const noexcept {
        return status == NutBoundedTailStatus::Exact;
    }
};

PcmPacketLayout deriveUncompressedPcmPacketLayout(
    const AVCodecParameters* codecpar) noexcept;

bool shouldProbeNutBoundedTailAuthority(
    const std::string& path,
    const AVFormatContext* formatContext,
    const AVStream* selectedAudioStream,
    bool strongerSampleExactAuthorityPresent) noexcept;

NutBoundedTailProbeResult probeNutBoundedTailAuthority(
    const std::string& path,
    int selectedStreamIndex,
    const NutBoundedTailProbeOptions& options = {});

const char* nutBoundedTailStatusName(NutBoundedTailStatus status) noexcept;

}  // namespace AveMediaBridge::Probe
