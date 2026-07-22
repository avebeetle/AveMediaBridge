#pragma once

#include "PresentationBudgetPolicy.hpp"
#include "../Ffmpeg/FfmpegHeaders.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace AveMediaBridge::Probe {

constexpr std::size_t kMatroskaAacSequentialReadBufferBytes = 256U * 1024U;

enum class MatroskaAacSequentialStatus {
    Exact,
    UnsupportedEarly,
    UnsupportedLate,
    Conflict,
    InvalidMedia,
    IoError
};

enum class MatroskaAacSequentialReason {
    NotEligible,
    CompleteSelectedTrackWithTerminalDiscard,
    CompleteSelectedTrackWithDeclaredZeroTerminalTrim,
    InputNotRegularFile,
    InputOpenFailed,
    InputReadFailed,
    ForcedReadError,
    InputChangedDuringScan,
    PhysicalEofNotReached,
    NotMatroska,
    UnsupportedDocType,
    EbmlHeaderMissing,
    InvalidEbmlHeader,
    SegmentMissing,
    SegmentDoesNotCoverFile,
    InvalidSegmentElement,
    ElementExceedsParent,
    MetadataElementTooLarge,
    InvalidInfo,
    InvalidTracks,
    TracksMissing,
    ClusterBeforeTracks,
    UnknownSizeCluster,
    InvalidCluster,
    TruncatedElement,
    InvalidOrDuplicateCrc,
    CrcMismatch,
    SelectedTrackMappingConflict,
    SelectedTrackAbsent,
    SelectedTrackHasNoBlocks,
    CodecIdUnsupported,
    CodecPrivateMismatch,
    AacConfigInvalid,
    AacProfileUnsupported,
    AacFrameLengthUnsupported,
    AacSampleRateConflict,
    AacChannelConflict,
    CodecDelayConflict,
    CodecDelayNotSampleExact,
    BlockReferencesUndeclaredTrack,
    InvalidBlockOrLacing,
    BlockPayloadTooLarge,
    DuplicateBlockInGroup,
    DuplicateBlockDuration,
    DuplicateDiscardPadding,
    BlockBeforeClusterTimestamp,
    BlockDurationConflict,
    SelectedTimestampGap,
    SelectedTimestampOverlap,
    SelectedTimestampOverflow,
    AccessUnitCountOverflow,
    DiscardPaddingBeforeFinalSelectedBlock,
    NegativeDiscardPaddingUnsupported,
    DiscardPaddingNotSampleExact,
    PhysicalFrameCountOverflow,
    TrimArithmeticOverflow,
    TrimExceedsPhysicalFrames,
    IndependentExactAuthorityConflict
};

struct MatroskaAacSelectedStreamIdentity {
    int streamIndex = -1;
    int audioOrdinal = -1;
    std::uint64_t trackNumber = 0;
    int sampleRate = 0;
    int channels = 0;
    int initialPadding = 0;
    std::vector<std::uint8_t> codecPrivate;
};

struct MatroskaAacSequentialEligibility {
    bool eligible = false;
    MatroskaAacSequentialReason reason = MatroskaAacSequentialReason::NotEligible;
    MatroskaAacSelectedStreamIdentity selected;
};

struct MatroskaAacSequentialTestHooks {
    std::uint64_t forceReadErrorAfterBytes =
        (std::numeric_limits<std::uint64_t>::max)();
};

struct MatroskaAacSequentialProbeOptions {
    std::size_t readBufferBytes = kMatroskaAacSequentialReadBufferBytes;
    const MatroskaAacSequentialTestHooks* testHooks = nullptr;
};

struct MatroskaAacSequentialPresentationResult {
    MatroskaAacSequentialStatus status =
        MatroskaAacSequentialStatus::UnsupportedEarly;
    MatroskaAacSequentialReason reason = MatroskaAacSequentialReason::NotEligible;

    std::uint64_t presentationFrames = 0;
    std::uint64_t physicalFrames = 0;
    std::uint64_t selectedAccessUnits = 0;
    std::uint64_t samplesPerAccessUnit = 0;
    std::uint64_t initialSkipFrames = 0;
    std::uint64_t terminalDiscardFrames = 0;
    std::uint64_t trackNumber = 0;
    std::uint64_t trackUid = 0;
    std::uint64_t codecDelayNanoseconds = 0;
    std::int64_t finalDiscardPaddingNanoseconds = 0;
    int sampleRate = 0;
    int channels = 0;
    int aacObjectType = 0;

    std::uint64_t fileSizeBytes = 0;
    std::uint64_t bytesReturned = 0;
    std::uint64_t uniqueBytes = 0;
    std::uint64_t duplicateBytes = 0;
    std::uint64_t readCalls = 0;
    std::uint64_t seekCallsAfterOpen = 0;
    std::uint64_t scanDurationUs = 0;
    std::uint64_t maximumWorkingBufferBytes = 0;
    std::uint64_t elementsParsed = 0;
    std::uint64_t clustersParsed = 0;
    std::uint64_t blocksParsed = 0;
    std::uint64_t selectedBlocks = 0;
    std::uint64_t selectedLaces = 0;
    std::uint64_t lacedSelectedBlocks = 0;
    std::uint64_t crcFailures = 0;

    bool reachedPhysicalEof = false;
    bool reachedSegmentEnd = false;
    bool segmentSizeUnknown = false;
    bool clusterSizeUnknown = false;
    bool selectedTrackMappingValid = false;
    bool timestampContinuityValid = true;
    bool allLacingValid = true;
    bool allRelevantCrcValid = true;
    bool checkedArithmeticValid = true;
    bool discardPaddingPresent = false;
    bool terminalZeroProvenByCompleteAbsence = false;
    bool cuesPresent = false;
    bool seekHeadPresent = false;

    bool exact() const noexcept {
        return status == MatroskaAacSequentialStatus::Exact;
    }
};

MatroskaAacSequentialEligibility evaluateMatroskaAacSequentialEligibility(
    const std::string& path,
    const AVFormatContext* formatContext,
    const AVStream* selectedAudioStream,
    bool strongerSampleExactAuthorityPresent);

MatroskaAacSequentialPresentationResult probeMatroskaAacSequentialPresentation(
    const std::string& path,
    const MatroskaAacSelectedStreamIdentity& selected,
    const MatroskaAacSequentialProbeOptions& options = {});

TotalPresentationEvidence makeMatroskaAacSequentialTotalPresentationEvidence(
    const MatroskaAacSequentialPresentationResult& result) noexcept;

const char* matroskaAacSequentialStatusName(
    MatroskaAacSequentialStatus status) noexcept;
const char* matroskaAacSequentialReasonName(
    MatroskaAacSequentialReason reason) noexcept;

}  // namespace AveMediaBridge::Probe
