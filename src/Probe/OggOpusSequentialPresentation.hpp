#pragma once

#include "PresentationBudgetPolicy.hpp"
#include "../Ffmpeg/FfmpegHeaders.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace AveMediaBridge::Probe {

constexpr std::size_t kOggOpusSequentialReadBufferBytes = 256U * 1024U;

enum class OggOpusSequentialStatus {
    Exact,
    Unsupported,
    Chained,
    Conflict,
    InvalidMedia,
    IoError
};

enum class OggOpusSequentialReason {
    NotEligible,
    CompleteContinuityProof,
    InputNotRegularFile,
    InputOpenFailed,
    InputInfoFailed,
    InputReadFailed,
    InputChangedDuringScan,
    PhysicalEofNotReached,
    TruncatedPageHeader,
    CapturePatternMissing,
    UnsupportedOggVersion,
    InvalidPageFlags,
    TruncatedSegmentTable,
    PageBodySizeOverflow,
    TruncatedPageBody,
    InvalidPageCrc,
    SerialDoesNotStartAtBosSequenceZero,
    SerialSequenceGap,
    DuplicateSerialBos,
    SerialPageAfterEos,
    PacketContinuationMismatch,
    IdentificationPacketTooLarge,
    InvalidOpusHead,
    UnsupportedOpusHeadVersion,
    SelectedStreamMappingConflict,
    SelectedOpusStreamAbsent,
    SelectedOpusTagsMissing,
    SelectedPacketTooLarge,
    InvalidOpusPacket,
    PacketDurationOverflow,
    IncompletePacketAtEof,
    ChainedOpusUnsupported,
    SelectedEosMissing,
    UnknownSelectedEosGranule,
    EosGranuleBeforePreSkip,
    SelectedAudioPacketsMissing,
    EosGranuleOutsideFinalPacketInterval,
    EmptyPresentation,
    IndependentExactAuthorityConflict,
    ForcedReadError
};

enum class OggOpusSequentialHandoffStatus {
    Accepted,
    Unavailable,
    Conflict
};

enum class OggOpusSequentialHandoffReason {
    AcceptedFastProbeEvidence,
    ProbeDocumentMissing,
    ProbeDocumentTooLarge,
    ProbeDocumentInvalid,
    SourceIdentityMismatch,
    SelectedStreamMismatch,
    EvidenceConflict
};

struct OggOpusPacketDurationResult {
    bool valid = false;
    int frameCount = 0;
    int samples48k = 0;
};

struct OggOpusSelectedStreamIdentity {
    int opusStreamOrdinal = -1;
    std::uint32_t logicalStreamCount = 0;
    int channels = 0;
    std::uint16_t preSkip = 0;
    std::uint32_t inputSampleRate = 0;
    std::int16_t outputGain = 0;
    std::uint8_t mappingFamily = 0;
    std::uint8_t streamCount = 0;
    std::uint8_t coupledCount = 0;
    std::vector<std::uint8_t> opusHead;
};

struct OggOpusSequentialEligibility {
    bool eligible = false;
    OggOpusSequentialReason reason = OggOpusSequentialReason::NotEligible;
    OggOpusSelectedStreamIdentity selected;
};

struct OggOpusSequentialTestHooks {
    std::uint64_t forceReadErrorAfterBytes =
        (std::numeric_limits<std::uint64_t>::max)();
    std::size_t selectedPacketMaximumOverride = 0;
};

struct OggOpusSequentialProbeOptions {
    std::size_t readBufferBytes = kOggOpusSequentialReadBufferBytes;
    const OggOpusSequentialTestHooks* testHooks = nullptr;
};

struct OggOpusSequentialPresentationResult {
    OggOpusSequentialStatus status = OggOpusSequentialStatus::Unsupported;
    OggOpusSequentialReason reason = OggOpusSequentialReason::NotEligible;

    std::uint64_t presentationFrames = 0;
    std::uint64_t physicalPacketFrames = 0;
    std::uint64_t eosGranule = 0;
    std::uint64_t preSkip = 0;
    std::uint64_t terminalDiscard = 0;
    std::uint64_t lastPacketDuration = 0;
    std::uint32_t selectedSerial = 0;

    std::uint64_t fileSizeBytes = 0;
    std::uint64_t fileIndex = 0;
    std::uint64_t lastWriteTime100ns = 0;
    std::uint32_t volumeSerialNumber = 0;
    std::uint64_t bytesReturned = 0;
    std::uint64_t uniqueBytes = 0;
    std::uint64_t duplicateBytes = 0;
    std::uint64_t readCalls = 0;
    std::uint64_t seekCallsAfterOpen = 0;
    std::uint64_t scanDurationUs = 0;
    std::uint64_t pagesParsed = 0;
    std::uint64_t selectedPages = 0;
    std::uint64_t selectedPackets = 0;
    std::uint64_t selectedAudioPackets = 0;
    std::uint64_t maximumPacketBytes = 0;
    std::uint64_t maximumWorkingBufferBytes = 0;

    bool reachedPhysicalEof = false;
    bool allPageCrcValid = true;
    bool selectedSequenceContinuous = true;
    bool packetContinuityValid = true;
    bool selectedEosGranuleKnown = false;
    bool finalGranuleInPacketInterval = false;

    bool exact() const noexcept {
        return status == OggOpusSequentialStatus::Exact;
    }
};

struct OggOpusSequentialHandoffResult {
    OggOpusSequentialHandoffStatus status =
        OggOpusSequentialHandoffStatus::Unavailable;
    OggOpusSequentialHandoffReason reason =
        OggOpusSequentialHandoffReason::ProbeDocumentMissing;
    OggOpusSequentialPresentationResult presentation;
    TotalPresentationEvidence evidence;

    bool accepted() const noexcept {
        return status == OggOpusSequentialHandoffStatus::Accepted;
    }
};

OggOpusPacketDurationResult parseOggOpusPacketDuration(
    const std::uint8_t* packet,
    std::size_t size,
    int streamCount) noexcept;

OggOpusSequentialEligibility evaluateOggOpusSequentialEligibility(
    const std::string& path,
    const AVFormatContext* formatContext,
    const AVStream* selectedAudioStream,
    bool strongerSampleExactAuthorityPresent);

OggOpusSequentialPresentationResult probeOggOpusSequentialPresentation(
    const std::string& path,
    const OggOpusSelectedStreamIdentity& selected,
    const OggOpusSequentialProbeOptions& options = {});

TotalPresentationEvidence makeOggOpusSequentialTotalPresentationEvidence(
    const OggOpusSequentialPresentationResult& result) noexcept;

OggOpusSequentialHandoffResult readOggOpusSequentialPresentationHandoff(
    const std::filesystem::path& probeJsonPath,
    const std::string& sourcePath,
    const AVFormatContext* formatContext,
    const AVStream* selectedAudioStream,
    const TotalPresentationEvidence& independentEvidence) noexcept;

const char* oggOpusSequentialStatusName(OggOpusSequentialStatus status) noexcept;
const char* oggOpusSequentialReasonName(OggOpusSequentialReason reason) noexcept;
const char* oggOpusSequentialHandoffStatusName(
    OggOpusSequentialHandoffStatus status) noexcept;
const char* oggOpusSequentialHandoffReasonName(
    OggOpusSequentialHandoffReason reason) noexcept;

}  // namespace AveMediaBridge::Probe
