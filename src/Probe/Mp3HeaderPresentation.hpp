#pragma once

#include "PresentationBudgetPolicy.hpp"

#include <cstdint>
#include <limits>
#include <string>

namespace AveMediaBridge::Probe {

constexpr std::uint64_t kMp3HeaderPresentationReadBudgetBytes = 256 * 1024;

enum class Mp3HeaderPresentationStatus {
    Exact,
    Unavailable,
    IoBudgetExceeded,
    Conflict,
    InvalidMedia
};

enum class Mp3HeaderType {
    None,
    Info,
    Xing,
    Vbri
};

struct Mp3HeaderValidationFlags {
    bool leadingTagValidated = false;
    bool trailingTagsValidated = false;
    bool firstFrameValidated = false;
    bool secondFrameValidated = false;
    bool byteRangeValidated = false;
    bool encoderProfileValidated = false;
    bool tagCrcValidated = false;
    bool gaplessFieldsValidated = false;
    bool checkedArithmetic = false;
};

struct Mp3HeaderPresentationProbeOptions {
    std::uint64_t hardReadBudgetBytes = kMp3HeaderPresentationReadBudgetBytes;
    bool forceInputNonSeekable = false;
    bool forceSeekFailure = false;
    std::uint64_t forceReadErrorAfterBytes =
        (std::numeric_limits<std::uint64_t>::max)();
};

struct Mp3HeaderPresentationResult {
    Mp3HeaderPresentationStatus status = Mp3HeaderPresentationStatus::Unavailable;
    Mp3HeaderType headerType = Mp3HeaderType::None;
    PresentationTotalTrust trust = PresentationTotalTrust::Unknown;
    PresentationTotalSource source = PresentationTotalSource::None;
    PresentationSampleDomain domain = PresentationSampleDomain::Unknown;
    std::string reason = "not_evaluated";
    std::string encoderProfile;
    int mpegVersion = 0;
    int layer = 0;
    int sampleRate = 0;
    int channels = 0;
    std::uint64_t physicalFrameCount = 0;
    std::uint64_t samplesPerFrame = 0;
    std::uint64_t physicalSampleTotal = 0;
    std::uint64_t initialPresentationSkip = 0;
    std::uint64_t terminalPresentationPadding = 0;
    std::uint64_t presentationFrames = 0;
    std::uint64_t declaredAudioBytes = 0;
    std::uint64_t audioDataBytes = 0;
    std::uint64_t fileSizeBytes = 0;
    std::uint64_t hardReadBudgetBytes = kMp3HeaderPresentationReadBudgetBytes;
    std::uint64_t totalActualReadBytes = 0;
    std::uint64_t uniqueBytesRead = 0;
    std::uint64_t duplicateBytesRead = 0;
    std::uint64_t maximumBudgetOverrunBytes = 0;
    std::uint64_t readCallCount = 0;
    std::uint64_t seekCallCount = 0;
    std::uint64_t maximumOffsetReached = 0;
    Mp3HeaderValidationFlags validation;

    bool exact() const noexcept {
        return status == Mp3HeaderPresentationStatus::Exact;
    }
};

bool shouldProbeMp3HeaderPresentation(
    const AVFormatContext* formatContext,
    const AVStream* audioStream,
    bool strongerExactAuthority) noexcept;

bool mp3HeaderPresentationMatchesStream(
    const Mp3HeaderPresentationResult& result,
    const AVStream* audioStream) noexcept;

Mp3HeaderPresentationResult probeMp3HeaderPresentation(
    const std::string& path,
    Mp3HeaderPresentationProbeOptions options = {});

const char* mp3HeaderPresentationStatusName(
    Mp3HeaderPresentationStatus status) noexcept;
const char* mp3HeaderTypeName(Mp3HeaderType type) noexcept;

}  // namespace AveMediaBridge::Probe
