#pragma once

#include "../Ffmpeg/FfmpegHeaders.hpp"

#include <cstdint>
#include <string>

namespace AveMediaBridge::Probe {

struct Mp3HeaderPresentationResult;

enum class PresentationTotalTrust {
    Unknown,
    Estimated,
    SampleExact
};

enum class PresentationTotalSource {
    None,
    StreamDurationEstimate,
    ExactPcmStreamDuration,
    OggEosGranule,
    FlacStreamInfoTotalSamples,
    NutBoundedTailSelectedStreamEnd,
    Mp3ValidatedHeaderPresentation,
    OggOpusSequentialPresentation,
    ExactPacketPresentation
};

enum class PresentationSampleDomain {
    Unknown,
    NativeStreamSamples
};

enum class PresentationTotalValidation {
    PacketCrossCheckRequired,
    SelfContainedMetadata
};

struct TotalPresentationEvidence {
    std::uint64_t frames = 0;
    PresentationTotalTrust trust = PresentationTotalTrust::Unknown;
    PresentationTotalSource source = PresentationTotalSource::None;
    PresentationSampleDomain domain = PresentationSampleDomain::Unknown;
    int sampleRate = 0;
    bool exactRescale = false;
    bool conflict = false;
    PresentationTotalValidation validation =
        PresentationTotalValidation::PacketCrossCheckRequired;
};

struct StreamingPresentationBudgetInput {
    TotalPresentationEvidence streamTotal;
    TotalPresentationEvidence exactPacketTotal;

    bool scanReachedEof = false;
    bool scanReadError = false;
    bool packetDurationsComplete = false;
    bool packetDurationArithmeticValid = false;
    std::int64_t packetDurationSumFrames = 0;
    std::int64_t initialSkipFrames = 0;
    std::int64_t terminalDiscardFrames = 0;
};

struct StreamingPresentationBudgetDecision {
    bool accepted = false;
    std::uint64_t frames = 0;
    PresentationTotalSource source = PresentationTotalSource::None;
    bool packetDurationCandidateKnown = false;
    std::uint64_t packetDurationCandidateFrames = 0;
    std::string rejectionReason = "no_evidence";
};

TotalPresentationEvidence makeStreamTotalPresentationEvidence(
    const AVFormatContext* formatContext,
    const AVStream* audioStream) noexcept;

TotalPresentationEvidence makeMp3HeaderTotalPresentationEvidence(
    const Mp3HeaderPresentationResult& result) noexcept;

TotalPresentationEvidence reconcileTotalPresentationEvidence(
    const TotalPresentationEvidence& sourceSpecific,
    const TotalPresentationEvidence& streamDuration) noexcept;

StreamingPresentationBudgetDecision resolveStreamingPresentationBudget(
    const StreamingPresentationBudgetInput& input);

std::uint64_t remainingPresentationInputFrames(
    std::uint64_t totalPresentationFrames,
    std::uint64_t acceptedInputFrames) noexcept;

const char* presentationTotalTrustName(PresentationTotalTrust trust) noexcept;
const char* presentationTotalSourceName(PresentationTotalSource source) noexcept;
const char* presentationSampleDomainName(PresentationSampleDomain domain) noexcept;
const char* presentationTotalValidationName(PresentationTotalValidation validation) noexcept;

}  // namespace AveMediaBridge::Probe
