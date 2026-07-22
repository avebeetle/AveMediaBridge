#pragma once

#include "PresentationBudgetPolicy.hpp"
#include "../Ffmpeg/FfmpegHeaders.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>

namespace AveMediaBridge::Probe {

constexpr std::uint64_t kMp4Mp3SampleTableReadBudgetBytes = 4U * 1024U * 1024U;

enum class Mp4Mp3SampleTableStatus {
    Exact,
    UnsupportedEarly,
    UnsupportedLate,
    Conflict,
    InvalidMedia,
    IoBudgetExceeded,
    IoError
};

enum class Mp4Mp3SampleTableReason {
    NotEligible,
    ValidatedClassicSampleEditTables,
    InputNotRegularFile,
    InputOpenFailed,
    InputReadFailed,
    InputSeekFailed,
    InputChangedDuringProbe,
    IoBudgetExceeded,
    NotMovMp4,
    SelectedStreamAbsent,
    SelectedCodecNotMp3,
    SelectedTrackIdInvalid,
    FragmentedMp4,
    MissingMoov,
    DuplicateMoov,
    MissingMdat,
    InvalidBox,
    UnsupportedExtendedNestedBox,
    ParserResourceLimit,
    SelectedTrackAbsent,
    DuplicateSelectedTrack,
    SelectedTrackNotAudio,
    InvalidMovieHeader,
    InvalidTrackHeader,
    InvalidMediaHeader,
    InvalidHandler,
    UnsupportedHeaderVersion,
    InvalidOrUnsupportedEditList,
    EditDurationNotSampleExact,
    EditTimelineConflict,
    UnsupportedSampleEntry,
    ProtectedSampleEntry,
    MultipleSampleDescriptions,
    ExternalDataReference,
    InvalidDataReference,
    InvalidEsds,
    UnsupportedMp3ObjectType,
    UnsupportedSampleSizeTable,
    UnsupportedChunkOffsetTable,
    InvalidStts,
    InvalidStsz,
    InvalidStsc,
    InvalidChunkOffsets,
    SampleCountConflict,
    DurationConflict,
    SampleDescriptionConflict,
    ChunkMappingConflict,
    ChunkOutsideMdat,
    ChunkOverlap,
    Mp3HeaderConflict,
    UnsupportedMp3Profile,
    VbrOrProfileChange,
    SampleSizeNotSingleCbrFrame,
    TerminalDiscardConflict,
    IndependentExactAuthorityConflict,
    ProbeDocumentMissing,
    ProbeDocumentInvalid,
    SourceIdentityMismatch
};

enum class Mp4Mp3SampleTableHandoffStatus {
    Accepted,
    Unavailable,
    Conflict
};

struct Mp4Mp3SampleTableProbeOptions {
    std::uint64_t hardReadBudgetBytes = kMp4Mp3SampleTableReadBudgetBytes;
    bool forceSeekFailure = false;
    std::uint64_t forceReadErrorAfterBytes =
        (std::numeric_limits<std::uint64_t>::max)();
};

struct Mp4Mp3SampleTableEligibility {
    bool eligible = false;
    Mp4Mp3SampleTableReason reason = Mp4Mp3SampleTableReason::NotEligible;
    int selectedStreamIndex = -1;
    std::uint32_t selectedTrackId = 0;
    int sampleRate = 0;
    int channels = 0;
};

struct Mp4Mp3SampleEditTablePresentationResult {
    Mp4Mp3SampleTableStatus status = Mp4Mp3SampleTableStatus::UnsupportedEarly;
    Mp4Mp3SampleTableReason reason = Mp4Mp3SampleTableReason::NotEligible;

    std::uint64_t presentationFrames = 0;
    std::uint64_t physicalFrames = 0;
    std::uint64_t initialSkipFrames = 0;
    std::uint64_t terminalDiscardFrames = 0;
    std::uint64_t editedMediaEnd = 0;
    std::uint64_t editMediaStart = 0;
    std::uint64_t editPresentationFrames = 0;

    int selectedStreamIndex = -1;
    std::uint32_t selectedTrackId = 0;
    std::uint64_t selectedSampleCount = 0;
    std::uint64_t samplesPerMp3Frame = 0;
    std::uint64_t mp3FramesPerMp4Sample = 0;
    std::uint32_t movieTimescale = 0;
    std::uint32_t mediaTimescale = 0;
    std::uint32_t editCount = 0;
    int mpegVersion = 0;
    int sampleRate = 0;
    int channels = 0;
    std::uint32_t sampleEntry = 0;
    std::uint8_t objectTypeIndication = 0;

    std::uint64_t fileSizeBytes = 0;
    std::uint64_t fileIndex = 0;
    std::uint64_t lastWriteTime100ns = 0;
    std::uint32_t volumeSerialNumber = 0;
    std::uint64_t hardReadBudgetBytes = kMp4Mp3SampleTableReadBudgetBytes;
    std::uint64_t bytesReturned = 0;
    std::uint64_t uniqueBytes = 0;
    std::uint64_t duplicateBytes = 0;
    std::uint64_t maximumBudgetOverrunBytes = 0;
    std::uint64_t readCalls = 0;
    std::uint64_t seekCalls = 0;
    std::uint64_t maximumOffsetReached = 0;
    std::uint64_t scanDurationUs = 0;
    std::uint64_t maximumWorkingBufferBytes = 0;
    std::uint64_t boxesParsed = 0;
    std::uint64_t tableEntriesParsed = 0;
    std::uint64_t selectedChunks = 0;
    std::uint64_t moovOffset = 0;
    std::uint64_t moovSize = 0;

    bool moovAtHead = false;
    bool moovAtTail = false;
    bool reachedRequiredMoovEnd = false;
    bool sampleInventoryValid = false;
    bool chunkMappingValid = false;
    bool chunkRangesInsideMdat = false;
    bool editListValid = false;
    bool mp3ProfileValid = false;
    bool checkedArithmeticValid = false;

    bool exact() const noexcept {
        return status == Mp4Mp3SampleTableStatus::Exact;
    }
};

struct Mp4Mp3SampleTableHandoffResult {
    Mp4Mp3SampleTableHandoffStatus status =
        Mp4Mp3SampleTableHandoffStatus::Unavailable;
    Mp4Mp3SampleTableReason reason =
        Mp4Mp3SampleTableReason::ProbeDocumentMissing;
    Mp4Mp3SampleEditTablePresentationResult presentation;
    TotalPresentationEvidence evidence;

    bool accepted() const noexcept {
        return status == Mp4Mp3SampleTableHandoffStatus::Accepted;
    }
};

Mp4Mp3SampleTableEligibility evaluateMp4Mp3SampleTableEligibility(
    const std::string& path,
    const AVFormatContext* formatContext,
    const AVStream* selectedAudioStream,
    bool strongerSampleExactAuthorityPresent) noexcept;

Mp4Mp3SampleEditTablePresentationResult
probeMp4Mp3SampleEditTablePresentation(
    const std::string& path,
    const Mp4Mp3SampleTableEligibility& eligibility,
    const Mp4Mp3SampleTableProbeOptions& options = {});

bool mp4Mp3SampleTableMatchesStream(
    const Mp4Mp3SampleEditTablePresentationResult& result,
    const AVStream* selectedAudioStream) noexcept;

TotalPresentationEvidence makeMp4Mp3SampleTableTotalPresentationEvidence(
    const Mp4Mp3SampleEditTablePresentationResult& result) noexcept;

Mp4Mp3SampleTableHandoffResult readMp4Mp3SampleTablePresentationHandoff(
    const std::filesystem::path& probeJsonPath,
    const std::string& sourcePath,
    const AVFormatContext* formatContext,
    const AVStream* selectedAudioStream,
    const TotalPresentationEvidence& independentEvidence) noexcept;

const char* mp4Mp3SampleTableStatusName(
    Mp4Mp3SampleTableStatus status) noexcept;
const char* mp4Mp3SampleTableReasonName(
    Mp4Mp3SampleTableReason reason) noexcept;
const char* mp4Mp3SampleTableHandoffStatusName(
    Mp4Mp3SampleTableHandoffStatus status) noexcept;

}  // namespace AveMediaBridge::Probe
