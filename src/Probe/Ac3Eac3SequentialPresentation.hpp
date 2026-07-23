#pragma once

#include "PresentationBudgetPolicy.hpp"
#include "../Ffmpeg/FfmpegHeaders.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>

namespace AveMediaBridge::Probe {

inline constexpr std::size_t kDolbySequentialReadBufferBytes = 262144;
inline constexpr std::uint64_t kAc3SamplesPerSyncframe = 1536;
inline constexpr std::uint64_t kEac3SamplesPerAudioBlock = 256;

enum class DolbySequentialCodecFamily {
    Unknown,
    Ac3,
    Eac3
};

enum class DolbySequentialPresentationStatus {
    ExactAc3,
    ExactEac3,
    UnsupportedEarly,
    UnsupportedLate,
    Conflict,
    InvalidMedia,
    IoError
};

enum class DolbySequentialPresentationReason {
    NotEligible,
    StrongerAuthorityPresent,
    NotStandaloneRawDolby,
    SelectedStreamMissing,
    CodecMismatch,
    OutputRateUnavailable,
    ChannelLayoutUnsupported,
    CodecPaddingUnsupported,
    InputNotRegularFile,
    InputOpenFailed,
    InputNotDiskFile,
    FileIdentityUnavailable,
    EmptyInput,
    InitialHeaderUnavailable,
    InitialSyncMissing,
    InvalidHeader,
    InvalidSyncAtFrameBoundary,
    TruncatedFinalHeader,
    TruncatedFinalFrame,
    UnsupportedBitstreamId,
    UnsupportedReducedRateEac3,
    UnsupportedEac3StreamType,
    UnsupportedEac3Substream,
    MixedCodecFamily,
    SampleRateConflict,
    ChannelConfigurationConflict,
    SampleRateChanged,
    ChannelConfigurationChanged,
    BitstreamIdChanged,
    UnexpectedTrailingData,
    CounterOverflow,
    PresentationOverflow,
    FileMutatedDuringScan,
    ReadError,
    ExactAuthorityConflict,
    CompleteStrictAc3Inventory,
    CompleteStrictEac3Inventory,
    ProbeDocumentMissing,
    ProbeDocumentInvalid,
    ProbeDocumentTooLarge,
    SourceIdentityMismatch,
    AcceptedFastProbeEvidence
};

enum class DolbySequentialHandoffStatus {
    Accepted,
    Unavailable,
    Conflict
};

struct DolbySelectedStreamIdentity {
    int streamIndex = -1;
    AVCodecID codecId = AV_CODEC_ID_NONE;
    int sampleRate = 0;
    int channels = 0;
};

struct DolbySequentialEligibility {
    bool eligible = false;
    DolbySequentialPresentationReason reason =
        DolbySequentialPresentationReason::NotEligible;
    DolbySelectedStreamIdentity selected;
};

struct DolbySequentialTestHooks {
    std::uint64_t forceReadErrorAfterBytes =
        (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t initialSyncframeCount = 0;
    std::uint64_t initialAudioBlockCount = 0;
    bool forceIdentityMismatchAtEnd = false;
};

struct DolbySequentialProbeOptions {
    std::size_t readBufferBytes = kDolbySequentialReadBufferBytes;
    const DolbySequentialTestHooks* testHooks = nullptr;
};

struct DolbySequentialPresentationResult {
    DolbySequentialPresentationStatus status =
        DolbySequentialPresentationStatus::UnsupportedEarly;
    DolbySequentialPresentationReason reason =
        DolbySequentialPresentationReason::NotEligible;
    DolbySequentialCodecFamily family = DolbySequentialCodecFamily::Unknown;

    std::uint64_t presentationFrames = 0;
    std::uint64_t syncframeCount = 0;
    std::uint64_t ac3FrameCount = 0;
    std::uint64_t eac3IndependentFrameCount = 0;
    std::uint64_t eac3DependentFrameCount = 0;
    std::uint64_t audioBlockCount = 0;
    std::uint64_t samplesPerAudioBlock = kEac3SamplesPerAudioBlock;

    int sampleRate = 0;
    int channels = 0;
    int bitstreamId = -1;
    int channelMode = -1;
    bool lfe = false;
    int selectedStreamType = -1;
    int selectedSubstreamId = -1;

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
    bool substreamPolicyValid = true;
    bool outputDomainValidated = false;
    bool checkedArithmeticValid = true;
    bool fileIdentityStable = false;
    bool crcObserved = false;
    bool crcValidated = false;
    bool payloadValiditySeparatedFromExtent = true;

    bool exact() const noexcept {
        return status == DolbySequentialPresentationStatus::ExactAc3 ||
            status == DolbySequentialPresentationStatus::ExactEac3;
    }
};

struct DolbySequentialHandoffResult {
    DolbySequentialHandoffStatus status =
        DolbySequentialHandoffStatus::Unavailable;
    DolbySequentialPresentationReason reason =
        DolbySequentialPresentationReason::ProbeDocumentMissing;
    DolbySequentialPresentationResult presentation;
    TotalPresentationEvidence evidence;

    bool accepted() const noexcept {
        return status == DolbySequentialHandoffStatus::Accepted;
    }
};

DolbySequentialEligibility evaluateDolbySequentialEligibility(
    const std::string& path,
    const AVFormatContext* formatContext,
    const AVStream* selectedAudioStream,
    bool strongerSampleExactAuthorityPresent);

DolbySequentialPresentationResult probeDolbySequentialPresentation(
    const std::string& path,
    const DolbySelectedStreamIdentity& selected,
    const DolbySequentialProbeOptions& options = {});

TotalPresentationEvidence makeDolbySequentialTotalPresentationEvidence(
    const DolbySequentialPresentationResult& result) noexcept;

DolbySequentialHandoffResult readDolbySequentialPresentationHandoff(
    const std::filesystem::path& probeJsonPath,
    const std::string& sourcePath,
    const AVFormatContext* formatContext,
    const AVStream* selectedAudioStream,
    const TotalPresentationEvidence& independentEvidence) noexcept;

const char* dolbySequentialCodecFamilyName(
    DolbySequentialCodecFamily family) noexcept;
const char* dolbySequentialStatusName(
    DolbySequentialPresentationStatus status) noexcept;
const char* dolbySequentialReasonName(
    DolbySequentialPresentationReason reason) noexcept;
const char* dolbySequentialHandoffStatusName(
    DolbySequentialHandoffStatus status) noexcept;

}  // namespace AveMediaBridge::Probe
