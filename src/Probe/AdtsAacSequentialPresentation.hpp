#pragma once

#include "PresentationBudgetPolicy.hpp"
#include "../Ffmpeg/FfmpegHeaders.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>

namespace AveMediaBridge::Probe {

inline constexpr std::size_t kAdtsAacSequentialReadBufferBytes = 262144;
inline constexpr std::uint64_t kAdtsAacSamplesPerRawDataBlock = 1024;

enum class AdtsAacSequentialStatus {
    Exact,
    UnsupportedEarly,
    UnsupportedLate,
    Conflict,
    InvalidMedia,
    IoError
};

enum class AdtsAacSequentialReason {
    NotEligible,
    StrongerAuthorityPresent,
    NotStandaloneAdts,
    SelectedStreamMissing,
    CodecNotAac,
    AacProfileUnsupported,
    AacFrameSizeUnsupported,
    OutputRateUnavailable,
    ChannelLayoutUnsupported,
    CodecPaddingUnsupported,
    InputNotRegularFile,
    InputOpenFailed,
    InputNotDiskFile,
    FileIdentityUnavailable,
    EmptyInput,
    InitialAdtsHeaderUnavailable,
    InitialSyncMissing,
    InvalidLayer,
    InvalidSampleRateIndex,
    InvalidFrameLength,
    CrcProtectedUnsupported,
    AacObjectTypeUnsupported,
    ProgramConfigElementUnsupported,
    MultipleRawDataBlocksUnsupported,
    SampleRateConflict,
    ChannelConfigurationConflict,
    MpegIdChanged,
    AacObjectTypeChanged,
    SampleRateChanged,
    ChannelConfigurationChanged,
    ProtectionModeChanged,
    RawDataBlockModeChanged,
    InvalidSyncAtFrameBoundary,
    TruncatedFinalHeader,
    TruncatedFinalPayload,
    TrailingTagUnsupported,
    UnexpectedTrailingData,
    CounterOverflow,
    PresentationOverflow,
    FileMutatedDuringScan,
    ReadError,
    ExactAuthorityConflict,
    CompleteStrictLcAdtsInventory,
    ProbeDocumentMissing,
    ProbeDocumentInvalid,
    ProbeDocumentTooLarge,
    SourceIdentityMismatch,
    AcceptedFastProbeEvidence
};

enum class AdtsAacSequentialHandoffStatus {
    Accepted,
    Unavailable,
    Conflict
};

struct AdtsAacSelectedStreamIdentity {
    int streamIndex = -1;
    int sampleRate = 0;
    int channels = 0;
    int codecProfile = -1;
    int codecFrameSize = 0;
};

struct AdtsAacSequentialEligibility {
    bool eligible = false;
    AdtsAacSequentialReason reason = AdtsAacSequentialReason::NotEligible;
    AdtsAacSelectedStreamIdentity selected;
};

struct AdtsAacSequentialTestHooks {
    std::uint64_t forceReadErrorAfterBytes =
        (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t initialFrameCount = 0;
    std::uint64_t initialRawDataBlockCount = 0;
    bool forceIdentityMismatchAtEnd = false;
};

struct AdtsAacSequentialProbeOptions {
    std::size_t readBufferBytes = kAdtsAacSequentialReadBufferBytes;
    const AdtsAacSequentialTestHooks* testHooks = nullptr;
};

struct AdtsAacSequentialPresentationResult {
    AdtsAacSequentialStatus status = AdtsAacSequentialStatus::UnsupportedEarly;
    AdtsAacSequentialReason reason = AdtsAacSequentialReason::NotEligible;

    std::uint64_t presentationFrames = 0;
    std::uint64_t physicalFrames = 0;
    std::uint64_t frameCount = 0;
    std::uint64_t rawDataBlockCount = 0;
    std::uint64_t samplesPerRawDataBlock = kAdtsAacSamplesPerRawDataBlock;

    int mpegId = -1;
    int audioObjectType = 0;
    int sampleRate = 0;
    int channels = 0;
    int channelConfiguration = 0;
    bool protectionAbsent = false;

    std::uint64_t fileSizeBytes = 0;
    std::uint64_t fileIndex = 0;
    std::uint64_t lastWriteTime100ns = 0;
    std::uint32_t volumeSerialNumber = 0;
    std::uint64_t bytesReturned = 0;
    std::uint64_t uniqueBytes = 0;
    std::uint64_t duplicateBytes = 0;
    std::uint64_t readCalls = 0;
    std::uint64_t seekCallsAfterOpen = 0;
    std::uint64_t maximumFrameBytes = 0;
    std::uint64_t scanDurationUs = 0;
    std::uint64_t maximumWorkingBufferBytes = 0;

    bool reachedPhysicalEof = false;
    bool frameBoundariesValid = true;
    bool configurationContinuous = true;
    bool outputDomainValidated = false;
    bool checkedArithmeticValid = true;
    bool fileIdentityStable = false;

    bool exact() const noexcept {
        return status == AdtsAacSequentialStatus::Exact;
    }
};

struct AdtsAacSequentialHandoffResult {
    AdtsAacSequentialHandoffStatus status =
        AdtsAacSequentialHandoffStatus::Unavailable;
    AdtsAacSequentialReason reason =
        AdtsAacSequentialReason::ProbeDocumentMissing;
    AdtsAacSequentialPresentationResult presentation;
    TotalPresentationEvidence evidence;

    bool accepted() const noexcept {
        return status == AdtsAacSequentialHandoffStatus::Accepted;
    }
};

AdtsAacSequentialEligibility evaluateAdtsAacSequentialEligibility(
    const std::string& path,
    const AVFormatContext* formatContext,
    const AVStream* selectedAudioStream,
    bool strongerSampleExactAuthorityPresent);

AdtsAacSequentialPresentationResult probeAdtsAacSequentialPresentation(
    const std::string& path,
    const AdtsAacSelectedStreamIdentity& selected,
    const AdtsAacSequentialProbeOptions& options = {});

TotalPresentationEvidence makeAdtsAacSequentialTotalPresentationEvidence(
    const AdtsAacSequentialPresentationResult& result) noexcept;

AdtsAacSequentialHandoffResult readAdtsAacSequentialPresentationHandoff(
    const std::filesystem::path& probeJsonPath,
    const std::string& sourcePath,
    const AVFormatContext* formatContext,
    const AVStream* selectedAudioStream,
    const TotalPresentationEvidence& independentEvidence) noexcept;

const char* adtsAacSequentialStatusName(AdtsAacSequentialStatus status) noexcept;
const char* adtsAacSequentialReasonName(AdtsAacSequentialReason reason) noexcept;
const char* adtsAacSequentialHandoffStatusName(
    AdtsAacSequentialHandoffStatus status) noexcept;

}  // namespace AveMediaBridge::Probe
