#include "PresentationBudgetPolicy.hpp"

#include "FlacStreamInfo.hpp"
#include "Mp3HeaderPresentation.hpp"

#include <algorithm>
#include <limits>
#include <string_view>

namespace AveMediaBridge::Probe {
namespace {

bool formatListContains(const char* formatNames, std::string_view expected) noexcept {
    if (!formatNames || expected.empty()) {
        return false;
    }

    std::string_view remaining(formatNames);
    while (!remaining.empty()) {
        const std::size_t comma = remaining.find(',');
        const std::string_view current = remaining.substr(0, comma);
        if (current == expected) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(comma + 1);
    }
    return false;
}

bool isPcmCodec(AVCodecID codecId) noexcept {
    const AVCodecDescriptor* descriptor = avcodec_descriptor_get(codecId);
    return descriptor && descriptor->type == AVMEDIA_TYPE_AUDIO &&
        (descriptor->props & AV_CODEC_PROP_LOSSLESS) != 0 &&
        codecId >= AV_CODEC_ID_FIRST_AUDIO && codecId < AV_CODEC_ID_ADPCM_IMA_QT;
}

bool exactNativeFrames(
    std::int64_t units,
    AVRational timeBase,
    int sampleRate,
    std::uint64_t& frames) noexcept {
    if (units == AV_NOPTS_VALUE || units < 0 ||
        timeBase.num <= 0 || timeBase.den <= 0 || sampleRate <= 0) {
        return false;
    }

    const AVRational sampleTimeBase{1, sampleRate};
    const std::int64_t candidate = av_rescale_q(units, timeBase, sampleTimeBase);
    if (candidate < 0 ||
        av_compare_ts(units, timeBase, candidate, sampleTimeBase) != 0) {
        return false;
    }
    frames = static_cast<std::uint64_t>(candidate);
    return true;
}

bool validExactTotal(const TotalPresentationEvidence& evidence) noexcept {
    return evidence.trust == PresentationTotalTrust::SampleExact &&
        evidence.domain == PresentationSampleDomain::NativeStreamSamples &&
        evidence.sampleRate > 0 &&
        evidence.exactRescale &&
        !evidence.conflict;
}

bool checkedPacketPresentation(
    const StreamingPresentationBudgetInput& input,
    std::uint64_t& frames) noexcept {
    if (!input.packetDurationsComplete ||
        !input.packetDurationArithmeticValid ||
        input.packetDurationSumFrames < 0 ||
        input.initialSkipFrames < 0 ||
        input.terminalDiscardFrames < 0) {
        return false;
    }

    std::int64_t remaining = input.packetDurationSumFrames;
    if (input.initialSkipFrames > remaining) {
        return false;
    }
    remaining -= input.initialSkipFrames;
    if (input.terminalDiscardFrames > remaining) {
        return false;
    }
    remaining -= input.terminalDiscardFrames;
    frames = static_cast<std::uint64_t>(remaining);
    return true;
}

}  // namespace

TotalPresentationEvidence reconcileTotalPresentationEvidence(
    const TotalPresentationEvidence& sourceSpecific,
    const TotalPresentationEvidence& streamDuration) noexcept {
    const bool sourceExact = validExactTotal(sourceSpecific);
    const bool durationExact = validExactTotal(streamDuration);
    if (sourceExact && durationExact) {
        TotalPresentationEvidence result = sourceSpecific;
        result.conflict =
            sourceSpecific.domain != streamDuration.domain ||
            sourceSpecific.sampleRate != streamDuration.sampleRate ||
            sourceSpecific.frames != streamDuration.frames;
        return result;
    }
    if (sourceExact) {
        return sourceSpecific;
    }
    return streamDuration;
}

TotalPresentationEvidence makeStreamTotalPresentationEvidence(
    const AVFormatContext* formatContext,
    const AVStream* audioStream) noexcept {
    TotalPresentationEvidence result;
    const AVCodecParameters* codecpar = audioStream ? audioStream->codecpar : nullptr;
    if (!codecpar) {
        return result;
    }

    TotalPresentationEvidence sourceSpecific;
    const FlacStreamInfoResult flac = parseFlacStreamInfo(codecpar);
    if (flac.valid()) {
        sourceSpecific.frames = flac.totalSamples;
        sourceSpecific.trust = PresentationTotalTrust::SampleExact;
        sourceSpecific.source = PresentationTotalSource::FlacStreamInfoTotalSamples;
        sourceSpecific.domain = PresentationSampleDomain::NativeStreamSamples;
        sourceSpecific.sampleRate = flac.sampleRate;
        sourceSpecific.exactRescale = true;
        sourceSpecific.validation = PresentationTotalValidation::SelfContainedMetadata;
    }

    std::uint64_t frames = 0;
    if (exactNativeFrames(
            audioStream->duration,
            audioStream->time_base,
            codecpar->sample_rate,
            frames)) {
        result.frames = frames;
        result.sampleRate = codecpar->sample_rate;
        result.domain = PresentationSampleDomain::NativeStreamSamples;
        result.exactRescale = true;
        result.trust = PresentationTotalTrust::Estimated;
        result.source = PresentationTotalSource::StreamDurationEstimate;
    }

    if (result.exactRescale && isPcmCodec(codecpar->codec_id)) {
        result.trust = PresentationTotalTrust::SampleExact;
        result.source = PresentationTotalSource::ExactPcmStreamDuration;
    }

    // The Ogg demuxer derives a Vorbis stream duration from the final-page
    // granule, whose native sample-domain value is the half-open PCM end.
    const bool oggVorbisGranule =
        formatContext && formatContext->iformat &&
        formatListContains(formatContext->iformat->name, "ogg") &&
        codecpar->codec_id == AV_CODEC_ID_VORBIS;
    if (result.exactRescale && oggVorbisGranule) {
        result.trust = PresentationTotalTrust::SampleExact;
        result.source = PresentationTotalSource::OggEosGranule;
    }
    return reconcileTotalPresentationEvidence(sourceSpecific, result);
}

TotalPresentationEvidence makeMp3HeaderTotalPresentationEvidence(
    const Mp3HeaderPresentationResult& result) noexcept {
    TotalPresentationEvidence evidence;
    if (!result.exact() || result.presentationFrames == 0 || result.sampleRate <= 0 ||
        result.trust != PresentationTotalTrust::SampleExact ||
        result.source != PresentationTotalSource::Mp3ValidatedHeaderPresentation ||
        result.domain != PresentationSampleDomain::NativeStreamSamples) {
        return evidence;
    }
    evidence.frames = result.presentationFrames;
    evidence.trust = result.trust;
    evidence.source = result.source;
    evidence.domain = result.domain;
    evidence.sampleRate = result.sampleRate;
    evidence.exactRescale = true;
    evidence.validation = PresentationTotalValidation::SelfContainedMetadata;
    return evidence;
}

StreamingPresentationBudgetDecision resolveStreamingPresentationBudget(
    const StreamingPresentationBudgetInput& input) {
    StreamingPresentationBudgetDecision result;
    const bool streamExact = validExactTotal(input.streamTotal);
    const bool packetExact = validExactTotal(input.exactPacketTotal);
    if ((input.streamTotal.trust == PresentationTotalTrust::SampleExact &&
         input.streamTotal.conflict) ||
        (input.exactPacketTotal.trust == PresentationTotalTrust::SampleExact &&
         input.exactPacketTotal.conflict)) {
        result.rejectionReason = "authoritative_total_conflict";
        return result;
    }
    if (streamExact && packetExact &&
        (input.streamTotal.sampleRate != input.exactPacketTotal.sampleRate ||
         input.streamTotal.frames != input.exactPacketTotal.frames)) {
        result.rejectionReason = "authoritative_total_conflict";
        return result;
    }

    if (streamExact &&
        input.streamTotal.validation == PresentationTotalValidation::SelfContainedMetadata) {
        result.accepted = true;
        result.frames = input.streamTotal.frames;
        result.source = input.streamTotal.source;
        result.rejectionReason = "accepted_self_contained_sample_exact_total";
        return result;
    }

    if (!input.scanReachedEof || input.scanReadError) {
        result.rejectionReason = "packet_scan_incomplete";
        return result;
    }

    std::uint64_t packetDurationCandidate = 0;
    result.packetDurationCandidateKnown =
        checkedPacketPresentation(input, packetDurationCandidate);
    result.packetDurationCandidateFrames = packetDurationCandidate;

    if (packetExact || streamExact) {
        const TotalPresentationEvidence& accepted =
            packetExact ? input.exactPacketTotal : input.streamTotal;
        result.accepted = true;
        result.frames = accepted.frames;
        result.source = accepted.source;
        result.rejectionReason = "accepted_sample_exact_total";
        return result;
    }

    if (input.streamTotal.trust != PresentationTotalTrust::Estimated ||
        input.streamTotal.domain != PresentationSampleDomain::NativeStreamSamples ||
        input.streamTotal.sampleRate <= 0 ||
        input.streamTotal.conflict ||
        !result.packetDurationCandidateKnown) {
        result.rejectionReason = "sample_exact_total_unavailable";
        return result;
    }
    if (packetDurationCandidate != input.streamTotal.frames) {
        result.rejectionReason = "packet_duration_validation_mismatch";
        return result;
    }

    result.accepted = true;
    result.frames = input.streamTotal.frames;
    result.source = input.streamTotal.source;
    result.rejectionReason = "accepted_packet_validated_estimate";
    return result;
}

std::uint64_t remainingPresentationInputFrames(
    std::uint64_t totalPresentationFrames,
    std::uint64_t acceptedInputFrames) noexcept {
    return totalPresentationFrames > acceptedInputFrames
        ? totalPresentationFrames - acceptedInputFrames
        : 0;
}

const char* presentationTotalTrustName(PresentationTotalTrust trust) noexcept {
    switch (trust) {
        case PresentationTotalTrust::Unknown:
            return "unknown";
        case PresentationTotalTrust::Estimated:
            return "estimated";
        case PresentationTotalTrust::SampleExact:
            return "sample_exact";
    }
    return "unknown";
}

const char* presentationTotalSourceName(PresentationTotalSource source) noexcept {
    switch (source) {
        case PresentationTotalSource::None:
            return "none";
        case PresentationTotalSource::StreamDurationEstimate:
            return "stream_duration_estimate";
        case PresentationTotalSource::ExactPcmStreamDuration:
            return "exact_pcm_stream_duration";
        case PresentationTotalSource::OggEosGranule:
            return "ogg_eos_granule";
        case PresentationTotalSource::FlacStreamInfoTotalSamples:
            return "flac_streaminfo_total_samples";
        case PresentationTotalSource::NutBoundedTailSelectedStreamEnd:
            return "nut_bounded_tail_selected_stream_end";
        case PresentationTotalSource::Mp3ValidatedHeaderPresentation:
            return "mp3_validated_header_presentation";
        case PresentationTotalSource::ExactPacketPresentation:
            return "exact_packet_presentation";
    }
    return "none";
}

const char* presentationSampleDomainName(PresentationSampleDomain domain) noexcept {
    switch (domain) {
        case PresentationSampleDomain::Unknown:
            return "unknown";
        case PresentationSampleDomain::NativeStreamSamples:
            return "native_stream_samples";
    }
    return "unknown";
}

const char* presentationTotalValidationName(
    PresentationTotalValidation validation) noexcept {
    switch (validation) {
        case PresentationTotalValidation::PacketCrossCheckRequired:
            return "packet_cross_check_required";
        case PresentationTotalValidation::SelfContainedMetadata:
            return "self_contained_metadata";
    }
    return "packet_cross_check_required";
}

}  // namespace AveMediaBridge::Probe
