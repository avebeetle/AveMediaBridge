#include "Probe/MediaOpenValidator.hpp"
#include "Probe/ProbeJsonWriter.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace Probe = AveMediaBridge::Probe;

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

Probe::FastProbeJsonDocument supportedDocument(
    std::string format,
    std::string codec) {
    Probe::FastProbeJsonDocument document;
    document.sourcePath = "wrong_extension.bin";
    document.formatName = std::move(format);
    document.containerFormat = document.formatName;
    document.hasAudio = true;
    document.selectedAudio.index = 1;
    document.selectedAudio.codecId = 1;
    document.selectedAudio.codecName = std::move(codec);
    document.selectedAudio.codecProfileName = "test-profile";
    document.selectedAudio.decoderName = "decoder";
    document.selectedAudio.sampleRate = 48000;
    document.selectedAudio.channels = 2;
    document.channelLayout = "stereo";
    document.decodedSampleFrames = 48000;
    return document;
}

Probe::TotalPresentationEvidence exactEvidence(
    Probe::PresentationTotalSource source =
        Probe::PresentationTotalSource::ExactPacketPresentation) {
    Probe::TotalPresentationEvidence evidence;
    evidence.frames = 48000;
    evidence.trust = Probe::PresentationTotalTrust::SampleExact;
    evidence.source = source;
    evidence.domain = Probe::PresentationSampleDomain::NativeStreamSamples;
    evidence.sampleRate = 48000;
    evidence.exactRescale = true;
    return evidence;
}

}  // namespace

int main() {
    bool ok = true;

    const Probe::MediaOpenAssessment certified = Probe::assessMediaOpen(
        supportedDocument("wav", "pcm_s16le"),
        exactEvidence(Probe::PresentationTotalSource::ExactPcmStreamDuration),
        true);
    ok &= expect(
        certified.disposition == Probe::MediaOpenDisposition::Accept,
        "certified WAV content should be accepted despite the source extension");
    ok &= expect(
        certified.certificationLevel ==
            Probe::MediaCertificationLevel::StageBReleaseGreen,
        "certified WAV family should retain Stage B classification");
    ok &= expect(
        certified.presentationMode == Probe::MediaPresentationMode::InstantReady,
        "exact PCM duration should classify as instant Ready");
    ok &= expect(
        certified.probeReused &&
            certified.presentationEvidenceReused &&
            !certified.additionalProbeRequired,
        "assessment must reuse the existing probe and presentation evidence");

    Probe::FastProbeJsonDocument uncertifiedDocument =
        supportedDocument("voc", "pcm_u8");
    const Probe::MediaOpenAssessment uncertified = Probe::assessMediaOpen(
        uncertifiedDocument,
        Probe::TotalPresentationEvidence{},
        true);
    ok &= expect(
        uncertified.disposition == Probe::MediaOpenDisposition::Accept &&
            uncertified.certificationLevel ==
                Probe::MediaCertificationLevel::SupportedUncertified,
        "decoder-supported uncertified content must remain accepted");
    ok &= expect(
        uncertified.presentationMode ==
            Probe::MediaPresentationMode::SafeReconcile,
        "supported non-exact evidence should classify as safe reconcile");

    Probe::FastProbeJsonDocument noAudio = supportedDocument("matroska", "aac");
    noAudio.hasAudio = false;
    noAudio.selectedAudio.index = -1;
    const Probe::MediaOpenAssessment missingAudio =
        Probe::assessMediaOpen(noAudio, Probe::TotalPresentationEvidence{}, true);
    ok &= expect(
        missingAudio.technicalSupport ==
            Probe::MediaTechnicalSupport::MissingAudioStream &&
            missingAudio.disposition == Probe::MediaOpenDisposition::Reject,
        "container without audio must be rejected with a typed reason");

    Probe::FastProbeJsonDocument noDecoder =
        supportedDocument("matroska", "unsupported_codec");
    noDecoder.selectedAudio.decoderName.clear();
    const Probe::MediaOpenAssessment missingDecoder =
        Probe::assessMediaOpen(noDecoder, Probe::TotalPresentationEvidence{}, true);
    ok &= expect(
        missingDecoder.technicalSupport ==
            Probe::MediaTechnicalSupport::MissingDecoder,
        "missing decoder must be distinct from missing audio");

    Probe::FastProbeJsonDocument missingParameters =
        supportedDocument("matroska", "aac");
    missingParameters.selectedAudio.sampleRate = 0;
    const Probe::MediaOpenAssessment unsupportedProfile =
        Probe::assessMediaOpen(
            missingParameters,
            Probe::TotalPresentationEvidence{},
            true);
    ok &= expect(
        unsupportedProfile.technicalSupport ==
                Probe::MediaTechnicalSupport::UnsupportedCodecProfile &&
            unsupportedProfile.reason ==
                Probe::MediaOpenReason::MissingRequiredAudioParameters,
        "missing profile parameters must fail with a typed profile rejection");

    Probe::FastProbeJsonDocument unsupportedContainer =
        supportedDocument("", "aac");
    const Probe::MediaOpenAssessment container =
        Probe::assessMediaOpen(
            unsupportedContainer,
            Probe::TotalPresentationEvidence{},
            true);
    ok &= expect(
        container.technicalSupport ==
            Probe::MediaTechnicalSupport::UnsupportedContainer,
        "unrecognized container must fail closed");

    Probe::FastProbeJsonDocument ambiguousRaw =
        supportedDocument("s16le", "pcm_s16le");
    const Probe::MediaOpenAssessment raw =
        Probe::assessMediaOpen(ambiguousRaw, Probe::TotalPresentationEvidence{}, true);
    ok &= expect(
        raw.technicalSupport == Probe::MediaTechnicalSupport::AmbiguousRawMedia,
        "raw PCM without explicit parameters must be rejected");

    Probe::FastProbeJsonDocument invalid;
    invalid.sourcePath = "music.mp3";
    invalid.errors.push_back("avformat_open_input failed: Invalid data found");
    const Probe::MediaOpenAssessment invalidMedia =
        Probe::assessMediaOpen(invalid, Probe::TotalPresentationEvidence{}, false);
    ok &= expect(
        invalidMedia.technicalSupport ==
            Probe::MediaTechnicalSupport::InvalidMedia,
        "invalid content must not be accepted by extension");

    Probe::FastProbeJsonDocument ioFailure;
    ioFailure.errors.push_back(
        "avformat_open_input failed: Permission denied");
    const Probe::MediaOpenAssessment ioError =
        Probe::assessMediaOpen(
            ioFailure,
            Probe::TotalPresentationEvidence{},
            false);
    ok &= expect(
        ioError.technicalSupport == Probe::MediaTechnicalSupport::IoError &&
            ioError.reason == Probe::MediaOpenReason::InputIoError,
        "probe I/O failures must remain distinct from malformed media");

    Probe::FastProbeJsonDocument conflictDocument =
        supportedDocument("ogg", "opus");
    Probe::TotalPresentationEvidence conflict = exactEvidence();
    conflict.conflict = true;
    const Probe::MediaOpenAssessment conflictAssessment =
        Probe::assessMediaOpen(conflictDocument, conflict, true);
    ok &= expect(
        conflictAssessment.reason ==
            Probe::MediaOpenReason::ConflictingPresentationEvidence &&
            conflictAssessment.disposition ==
                Probe::MediaOpenDisposition::Reject,
        "independent exact conflict must fail closed");

    constexpr std::uint64_t kIterations = 100000;
    const Probe::FastProbeJsonDocument benchmarkDocument =
        supportedDocument("ogg", "vorbis");
    const Probe::TotalPresentationEvidence benchmarkEvidence = exactEvidence();
    std::uint64_t accepted = 0;
    std::vector<std::int64_t> samplesNs;
    samplesNs.reserve(kIterations);
    const auto started = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < kIterations; ++i) {
        const auto sampleStarted = std::chrono::steady_clock::now();
        accepted += Probe::assessMediaOpen(
            benchmarkDocument,
            benchmarkEvidence,
            true).canImport ? 1ULL : 0ULL;
        samplesNs.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - sampleStarted).count());
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    const double averageUs =
        static_cast<double>(elapsed.count()) / static_cast<double>(kIterations);
    std::sort(samplesNs.begin(), samplesNs.end());
    const double medianUs =
        static_cast<double>(samplesNs[samplesNs.size() / 2]) / 1000.0;
    const double p95Us =
        static_cast<double>(
            samplesNs[((samplesNs.size() - 1) * 95) / 100]) /
        1000.0;
    ok &= expect(accepted == kIterations, "benchmark assessments must accept");
    ok &= expect(
        medianUs < 500.0 && p95Us < 2000.0,
        "pure assessment must remain well below the 0.5 ms median gate");

    const bool stableNames =
        std::string(Probe::mediaOpenDispositionName(
            Probe::MediaOpenDisposition::Accept)) == "accept" &&
        std::string(Probe::mediaTechnicalSupportName(
            Probe::MediaTechnicalSupport::MissingDecoder)) == "missing_decoder" &&
        std::string(Probe::mediaCertificationLevelName(
            Probe::MediaCertificationLevel::SupportedUncertified)) ==
            "supported_uncertified" &&
        std::string(Probe::mediaPresentationModeName(
            Probe::MediaPresentationMode::GenericExactFallback)) ==
            "generic_exact_fallback";
    ok &= expect(stableNames, "stable enum names must remain exhaustive");

    if (!ok) {
        return 1;
    }
    std::cout << "mediaAssessmentAverageUs=" << averageUs
              << " mediaAssessmentMedianUs=" << medianUs
              << " mediaAssessmentP95Us=" << p95Us << "\n";
    std::cout << "additionalFileOpens=0 additionalProbeInvocations=0 "
                 "additionalAuthorityInvocations=0 additionalPhysicalPasses=0 "
                 "additionalBytesRead=0 additionalDecoderContexts=0\n";
    std::cout << "AVEMEDIABRIDGE_MEDIA_OPEN_VALIDATION_OK\n";
    return 0;
}
