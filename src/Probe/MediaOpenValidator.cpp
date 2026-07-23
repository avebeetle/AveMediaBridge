#include "MediaOpenValidator.hpp"

#include "ProbeJsonWriter.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <initializer_list>
#include <string_view>

namespace AveMediaBridge::Probe {
namespace {

bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const auto left = static_cast<unsigned char>(lhs[i]);
        const auto right = static_cast<unsigned char>(rhs[i]);
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

bool hasFormatToken(std::string_view formatName, std::string_view expected) {
    std::size_t start = 0;
    while (start <= formatName.size()) {
        const std::size_t comma = formatName.find(',', start);
        const std::size_t end =
            comma == std::string_view::npos ? formatName.size() : comma;
        if (equalsIgnoreCase(formatName.substr(start, end - start), expected)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return false;
}

bool hasAnyFormatToken(
    std::string_view formatName,
    std::initializer_list<std::string_view> expected) {
    return std::any_of(
        expected.begin(),
        expected.end(),
        [&](std::string_view value) {
            return hasFormatToken(formatName, value);
        });
}

bool isRawPcmDemuxer(std::string_view formatName) {
    constexpr std::array<std::string_view, 18> kRawPcmFormats{
        "s8", "u8", "s16le", "s16be", "u16le", "u16be",
        "s24le", "s24be", "u24le", "u24be", "s32le", "s32be",
        "u32le", "u32be", "f32le", "f32be", "f64le", "f64be"};
    return std::any_of(
        kRawPcmFormats.begin(),
        kRawPcmFormats.end(),
        [&](std::string_view value) {
            return hasFormatToken(formatName, value);
        });
}

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() &&
        value.substr(0, prefix.size()) == prefix;
}

bool isStageBReleaseGreenFamily(const FastProbeJsonDocument& document) {
    const std::string_view format = document.formatName;
    const std::string_view codec = document.selectedAudio.codecName;

    if (startsWith(codec, "pcm_")) {
        return hasAnyFormatToken(
            format,
            {"wav", "w64", "aiff", "caf", "nut", "mov", "matroska"});
    }
    if (codec == "flac") {
        return hasAnyFormatToken(format, {"flac", "ogg", "matroska"});
    }
    if (codec == "alac") {
        return hasAnyFormatToken(format, {"mov", "caf", "matroska"});
    }
    if (codec == "mp3") {
        return hasAnyFormatToken(format, {"mp3", "mov", "matroska"});
    }
    if (codec == "aac") {
        return hasAnyFormatToken(
            format,
            {"aac", "mov", "matroska", "mpegts", "mpeg"});
    }
    if (codec == "vorbis" || codec == "opus") {
        return hasAnyFormatToken(format, {"ogg", "matroska", "webm"});
    }
    if (codec == "ac3" || codec == "eac3") {
        return hasAnyFormatToken(
            format,
            {"ac3", "eac3", "mov", "matroska", "mpeg", "mpegts"});
    }
    return false;
}

bool containsIoFailure(const FastProbeJsonDocument& document) {
    return std::any_of(
        document.errors.begin(),
        document.errors.end(),
        [](const std::string& error) {
            return error.find("Permission denied") != std::string::npos ||
                error.find("Access is denied") != std::string::npos ||
                error.find("Input/output error") != std::string::npos ||
                error.find("I/O error") != std::string::npos;
        });
}

MediaPresentationMode presentationModeFor(
    const FastProbeJsonDocument& document,
    const TotalPresentationEvidence& evidence) {
    if (evidence.trust == PresentationTotalTrust::SampleExact) {
        if (evidence.source == PresentationTotalSource::ExactPcmStreamDuration ||
            evidence.source == PresentationTotalSource::FlacStreamInfoTotalSamples) {
            return MediaPresentationMode::InstantReady;
        }
        if (evidence.source == PresentationTotalSource::ExactPacketPresentation) {
            return MediaPresentationMode::GenericExactFallback;
        }
        return MediaPresentationMode::SampleExact;
    }
    return MediaPresentationMode::SafeReconcile;
}

MediaOpenAssessment reject(
    const FastProbeJsonDocument& document,
    const TotalPresentationEvidence& evidence,
    MediaTechnicalSupport support,
    MediaOpenReason reason,
    const char* messageKey) {
    MediaOpenAssessment result;
    result.technicalSupport = support;
    result.reason = reason;
    result.userMessageKey = messageKey;
    result.detectedContainer = document.containerFormat;
    result.detectedCodec = document.selectedAudio.codecName;
    result.detectedProfile = document.selectedAudio.codecProfileName;
    result.audioStreamIndex = document.selectedAudio.index;
    result.sampleRate = document.selectedAudio.sampleRate;
    result.channels = document.selectedAudio.channels;
    result.channelLayout = document.channelLayout;
    result.decoderAvailable = !document.selectedAudio.decoderName.empty();
    result.selectedAudioStreamFound =
        document.hasAudio && document.selectedAudio.index >= 0;
    result.authoritySource = evidence.source;
    result.authorityTrust = evidence.trust;
    result.authorityDomain = evidence.domain;
    return result;
}

}  // namespace

MediaOpenAssessment assessMediaOpen(
    const FastProbeJsonDocument& document,
    const TotalPresentationEvidence& totalPresentation,
    bool streamInfoFound) {
    if (!streamInfoFound) {
        if (containsIoFailure(document)) {
            return reject(
                document,
                totalPresentation,
                MediaTechnicalSupport::IoError,
                MediaOpenReason::InputIoError,
                "media.io_error");
        }
        return reject(
            document,
            totalPresentation,
            MediaTechnicalSupport::InvalidMedia,
            MediaOpenReason::InvalidMedia,
            "media.invalid");
    }
    if (document.formatName.empty()) {
        return reject(
            document,
            totalPresentation,
            MediaTechnicalSupport::UnsupportedContainer,
            MediaOpenReason::UnsupportedContainer,
            "media.unsupported_container");
    }
    if (!document.hasAudio || document.selectedAudio.index < 0) {
        return reject(
            document,
            totalPresentation,
            MediaTechnicalSupport::MissingAudioStream,
            MediaOpenReason::MissingAudioStream,
            "media.missing_audio");
    }
    if (isRawPcmDemuxer(document.formatName)) {
        return reject(
            document,
            totalPresentation,
            MediaTechnicalSupport::AmbiguousRawMedia,
            MediaOpenReason::AmbiguousRawMedia,
            "media.ambiguous_raw");
    }
    if (document.selectedAudio.codecId == 0 ||
        document.selectedAudio.codecName.empty() ||
        document.selectedAudio.codecName == "unknown") {
        return reject(
            document,
            totalPresentation,
            MediaTechnicalSupport::UnsupportedAudioCodec,
            MediaOpenReason::UnsupportedAudioCodec,
            "media.unsupported_codec");
    }
    if (document.selectedAudio.decoderName.empty()) {
        return reject(
            document,
            totalPresentation,
            MediaTechnicalSupport::MissingDecoder,
            MediaOpenReason::MissingDecoder,
            "media.missing_decoder");
    }
    if (document.selectedAudio.sampleRate <= 0 ||
        document.selectedAudio.channels <= 0) {
        return reject(
            document,
            totalPresentation,
            MediaTechnicalSupport::UnsupportedCodecProfile,
            MediaOpenReason::MissingRequiredAudioParameters,
            "media.missing_audio_parameters");
    }
    if (totalPresentation.conflict) {
        return reject(
            document,
            totalPresentation,
            MediaTechnicalSupport::InvalidMedia,
            MediaOpenReason::ConflictingPresentationEvidence,
            "media.presentation_conflict");
    }

    MediaOpenAssessment result;
    result.disposition = MediaOpenDisposition::Accept;
    result.technicalSupport = MediaTechnicalSupport::Supported;
    result.certificationLevel = isStageBReleaseGreenFamily(document)
        ? MediaCertificationLevel::StageBReleaseGreen
        : MediaCertificationLevel::SupportedUncertified;
    result.presentationMode =
        presentationModeFor(document, totalPresentation);
    result.reason =
        result.certificationLevel == MediaCertificationLevel::StageBReleaseGreen
        ? MediaOpenReason::AcceptedCertified
        : MediaOpenReason::AcceptedSupportedUncertified;
    result.userMessageKey =
        result.certificationLevel == MediaCertificationLevel::StageBReleaseGreen
        ? "media.accepted_certified"
        : "media.accepted_uncertified";
    result.detectedContainer = document.containerFormat;
    result.detectedCodec = document.selectedAudio.codecName;
    result.detectedProfile = document.selectedAudio.codecProfileName;
    result.audioStreamIndex = document.selectedAudio.index;
    result.sampleRate = document.selectedAudio.sampleRate;
    result.channels = document.selectedAudio.channels;
    result.channelLayout = document.channelLayout;
    result.decoderAvailable = true;
    result.selectedAudioStreamFound = true;
    result.canImport = true;
    result.sampleExactLoadingAvailable =
        totalPresentation.trust == PresentationTotalTrust::SampleExact;
    result.safeReconcileRequired =
        result.presentationMode == MediaPresentationMode::SafeReconcile;
    result.authoritySource = totalPresentation.source;
    result.authorityTrust = totalPresentation.trust;
    result.authorityDomain = totalPresentation.domain;
    return result;
}

const char* mediaOpenDispositionName(MediaOpenDisposition value) noexcept {
    switch (value) {
    case MediaOpenDisposition::Accept: return "accept";
    case MediaOpenDisposition::Reject: return "reject";
    }
    return "reject";
}

const char* mediaTechnicalSupportName(MediaTechnicalSupport value) noexcept {
    switch (value) {
    case MediaTechnicalSupport::Supported: return "supported";
    case MediaTechnicalSupport::MissingAudioStream: return "missing_audio_stream";
    case MediaTechnicalSupport::UnsupportedContainer: return "unsupported_container";
    case MediaTechnicalSupport::UnsupportedAudioCodec: return "unsupported_audio_codec";
    case MediaTechnicalSupport::UnsupportedCodecProfile: return "unsupported_codec_profile";
    case MediaTechnicalSupport::MissingDecoder: return "missing_decoder";
    case MediaTechnicalSupport::AmbiguousRawMedia: return "ambiguous_raw_media";
    case MediaTechnicalSupport::InvalidMedia: return "invalid_media";
    case MediaTechnicalSupport::IoError: return "io_error";
    }
    return "invalid_media";
}

const char* mediaCertificationLevelName(MediaCertificationLevel value) noexcept {
    switch (value) {
    case MediaCertificationLevel::StageBReleaseGreen: return "stage_b_release_green";
    case MediaCertificationLevel::TargetedCertified: return "targeted_certified";
    case MediaCertificationLevel::SupportedUncertified: return "supported_uncertified";
    case MediaCertificationLevel::NotApplicable: return "not_applicable";
    }
    return "not_applicable";
}

const char* mediaPresentationModeName(MediaPresentationMode value) noexcept {
    switch (value) {
    case MediaPresentationMode::SampleExact: return "sample_exact";
    case MediaPresentationMode::SafeReconcile: return "safe_reconcile";
    case MediaPresentationMode::InstantReady: return "instant_ready";
    case MediaPresentationMode::GenericExactFallback: return "generic_exact_fallback";
    case MediaPresentationMode::Unknown: return "unknown";
    }
    return "unknown";
}

const char* mediaOpenReasonName(MediaOpenReason value) noexcept {
    switch (value) {
    case MediaOpenReason::AcceptedCertified: return "accepted_certified";
    case MediaOpenReason::AcceptedSupportedUncertified: return "accepted_supported_uncertified";
    case MediaOpenReason::InputIoError: return "input_io_error";
    case MediaOpenReason::InvalidMedia: return "invalid_media";
    case MediaOpenReason::UnsupportedContainer: return "unsupported_container";
    case MediaOpenReason::MissingAudioStream: return "missing_audio_stream";
    case MediaOpenReason::UnsupportedAudioCodec: return "unsupported_audio_codec";
    case MediaOpenReason::UnsupportedCodecProfile: return "unsupported_codec_profile";
    case MediaOpenReason::MissingDecoder: return "missing_decoder";
    case MediaOpenReason::AmbiguousRawMedia: return "ambiguous_raw_media";
    case MediaOpenReason::MissingRequiredAudioParameters: return "missing_required_audio_parameters";
    case MediaOpenReason::ConflictingPresentationEvidence: return "conflicting_presentation_evidence";
    }
    return "invalid_media";
}

}  // namespace AveMediaBridge::Probe
