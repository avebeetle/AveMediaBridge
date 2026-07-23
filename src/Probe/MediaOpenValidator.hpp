#pragma once

#include "PresentationBudgetPolicy.hpp"

#include <string>

namespace AveMediaBridge::Probe {

struct FastProbeJsonDocument;

enum class MediaOpenDisposition {
    Accept,
    Reject
};

enum class MediaTechnicalSupport {
    Supported,
    MissingAudioStream,
    UnsupportedContainer,
    UnsupportedAudioCodec,
    UnsupportedCodecProfile,
    MissingDecoder,
    AmbiguousRawMedia,
    InvalidMedia,
    IoError
};

enum class MediaCertificationLevel {
    StageBReleaseGreen,
    TargetedCertified,
    SupportedUncertified,
    NotApplicable
};

enum class MediaPresentationMode {
    SampleExact,
    SafeReconcile,
    InstantReady,
    GenericExactFallback,
    Unknown
};

enum class MediaOpenReason {
    AcceptedCertified,
    AcceptedSupportedUncertified,
    InputIoError,
    InvalidMedia,
    UnsupportedContainer,
    MissingAudioStream,
    UnsupportedAudioCodec,
    UnsupportedCodecProfile,
    MissingDecoder,
    AmbiguousRawMedia,
    MissingRequiredAudioParameters,
    ConflictingPresentationEvidence
};

struct MediaOpenAssessment {
    MediaOpenDisposition disposition = MediaOpenDisposition::Reject;
    MediaTechnicalSupport technicalSupport =
        MediaTechnicalSupport::InvalidMedia;
    MediaCertificationLevel certificationLevel =
        MediaCertificationLevel::NotApplicable;
    MediaPresentationMode presentationMode =
        MediaPresentationMode::Unknown;
    MediaOpenReason reason = MediaOpenReason::InvalidMedia;

    std::string detectedContainer;
    std::string detectedCodec;
    std::string detectedProfile;
    int audioStreamIndex = -1;
    int sampleRate = 0;
    int channels = 0;
    std::string channelLayout;

    bool decoderAvailable = false;
    bool selectedAudioStreamFound = false;
    bool canImport = false;
    bool sampleExactLoadingAvailable = false;
    bool safeReconcileRequired = false;

    PresentationTotalSource authoritySource = PresentationTotalSource::None;
    PresentationTotalTrust authorityTrust = PresentationTotalTrust::Unknown;
    PresentationSampleDomain authorityDomain =
        PresentationSampleDomain::Unknown;

    bool probeReused = true;
    bool presentationEvidenceReused = true;
    bool additionalProbeRequired = false;
    const char* userMessageKey = "media.invalid";
};

MediaOpenAssessment assessMediaOpen(
    const FastProbeJsonDocument& document,
    const TotalPresentationEvidence& totalPresentation,
    bool streamInfoFound);

const char* mediaOpenDispositionName(MediaOpenDisposition value) noexcept;
const char* mediaTechnicalSupportName(MediaTechnicalSupport value) noexcept;
const char* mediaCertificationLevelName(MediaCertificationLevel value) noexcept;
const char* mediaPresentationModeName(MediaPresentationMode value) noexcept;
const char* mediaOpenReasonName(MediaOpenReason value) noexcept;

}  // namespace AveMediaBridge::Probe
