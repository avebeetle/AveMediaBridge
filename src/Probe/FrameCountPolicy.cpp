#include "FrameCountPolicy.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <sstream>

namespace AveMediaBridge::Probe {
namespace {

std::int64_t frameDeltaAbs(std::int64_t left, std::int64_t right) {
    return left >= right ? left - right : right - left;
}

bool formatNameContains(const std::string& formatName, const char* token) {
    return formatName.find(token) != std::string::npos;
}

bool isPacketTimelinePolicyContainer(const std::string& formatName) {
    return formatNameContains(formatName, "mpegts") ||
        formatNameContains(formatName, "flv");
}

bool isMpegTsContainer(const std::string& formatName) {
    return formatNameContains(formatName, "mpegts");
}

bool isMovMp4FamilyContainer(const std::string& formatName) {
    return formatNameContains(formatName, "mov") ||
        formatNameContains(formatName, "mp4") ||
        formatNameContains(formatName, "m4a") ||
        formatNameContains(formatName, "3gp") ||
        formatNameContains(formatName, "3g2") ||
        formatNameContains(formatName, "mj2");
}

bool isAsfContainer(const std::string& formatName) {
    return formatNameContains(formatName, "asf");
}

bool isOggContainer(const std::string& formatName) {
    return formatNameContains(formatName, "ogg");
}

bool isMpegPsContainer(const std::string& formatName) {
    return formatName == "mpeg";
}

bool isBareMp3Container(const std::string& formatName) {
    return formatName == "mp3";
}

bool isMp3AudioCodec(const FrameCountPolicyState& state) {
    return state.selectedAudio.codecId == AV_CODEC_ID_MP3 ||
        state.selectedAudio.codecName == "mp3";
}

bool isAacAudioCodec(const FrameCountPolicyState& state) {
    return state.selectedAudio.codecId == AV_CODEC_ID_AAC ||
        state.selectedAudio.codecName == "aac";
}

bool isRawAdtsAac(const FrameCountPolicyState& state) {
    return state.formatName == "aac" && isAacAudioCodec(state);
}

bool isOpusAudioCodec(const FrameCountPolicyState& state) {
    return state.selectedAudio.codecId == AV_CODEC_ID_OPUS ||
        state.selectedAudio.codecName == "opus";
}

bool isVorbisAudioCodec(const FrameCountPolicyState& state) {
    return state.selectedAudio.codecId == AV_CODEC_ID_VORBIS ||
        state.selectedAudio.codecName == "vorbis";
}

bool isMp2AudioCodec(const FrameCountPolicyState& state) {
    return state.selectedAudio.codecId == AV_CODEC_ID_MP2 ||
        state.selectedAudio.codecName == "mp2";
}

bool isWmav2AudioCodec(const FrameCountPolicyState& state) {
    return state.selectedAudio.codecId == AV_CODEC_ID_WMAV2 ||
        state.selectedAudio.codecName == "wmav2";
}

void updateEstimatedDecodedBytes(FrameCountPolicyState& state) {
    if (state.decodedSampleFrames > 0 && state.selectedAudio.channels > 0) {
        state.estimatedDecodedBytes =
            state.decodedSampleFrames *
            static_cast<std::int64_t>(state.selectedAudio.channels) *
            static_cast<std::int64_t>(sizeof(float));
        state.estimatedDecodedBytesKind = state.decodedSampleFramesKind;
    }
}

bool auditEnabled() {
    const char* value = std::getenv("AVEMEDIABRIDGE_PCM_EXTENT_AUDIT");
    if (!value || value[0] == '\0') {
        return false;
    }
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text != "0" && text != "false" && text != "off" && text != "no";
}

const char* boolText(bool value) {
    return value ? "yes" : "no";
}

void applyOggVorbisExactExtentPolicyInternal(FrameCountPolicyState& state) {
    if (state.gaplessCorrectionApplied ||
        state.decodedSampleFramesKind == "exact" ||
        !isOggContainer(state.formatName) ||
        !isVorbisAudioCodec(state) ||
        state.selectedAudio.sampleRate <= 0 ||
        state.decodedSampleFrames <= 0 ||
        state.streamDurationFrames <= 0 ||
        state.decodedSampleFramesSource != "stream_duration_estimate" ||
        state.streamDurationFrames != state.decodedSampleFrames ||
        (state.packetPtsSpanFrames > 0 && state.packetPtsSpanFrames != state.decodedSampleFrames) ||
        (state.packetDurationSumFrames > 0 && state.packetDurationSumFrames != state.decodedSampleFrames) ||
        state.skipSamplesTotal != 0 ||
        !state.oggVorbisTerminalScanAvailable ||
        !state.oggVorbisTerminalScanComplete ||
        state.oggVorbisTruncated ||
        state.oggVorbisChainedOrAmbiguous ||
        state.oggVorbisTimestampDiscontinuity ||
        !state.oggVorbisEosObserved ||
        !state.oggVorbisEosGranuleKnown ||
        state.oggVorbisEosGranuleFrames <= 0 ||
        state.oggVorbisEosGranuleFrames != state.decodedSampleFrames) {
        return;
    }

    state.decodedSampleFramesBeforeCorrection = state.decodedSampleFrames;
    state.decodedSampleFramesKind = "exact";
    state.decodedSampleFramesTrust = "authoritative";
    state.decodedSampleFramesSource = "ogg_vorbis_eos_granule";
    state.frameCountPolicyReason = "ogg_vorbis_exact_extent_proven";
    updateEstimatedDecodedBytes(state);
}

void applyOpusFrameCountPolicy(FrameCountPolicyState& state, const PacketFrameCountScan& scan) {
    if (state.gaplessCorrectionApplied ||
        state.decodedSampleFramesKind == "exact" ||
        !isOpusAudioCodec(state) ||
        state.skipSamplesTotal <= 0 ||
        scan.audioPacketCount <= 0) {
        return;
    }

    const std::int64_t opusPaddingToApply =
        isOggContainer(state.formatName)
            ? state.skipSamplesStart
            : state.skipSamplesTotal;
    std::int64_t correctedFrames = 0;
    if (scan.packetDurationSumFrames > opusPaddingToApply) {
        correctedFrames = scan.packetDurationSumFrames - opusPaddingToApply;
    } else if (scan.packetPtsSpanWithoutLastDurationFrames > 0) {
        correctedFrames = scan.packetPtsSpanWithoutLastDurationFrames;
    }
    if (correctedFrames <= 0) {
        return;
    }

    state.gaplessCorrectionApplied = true;
    state.decodedSampleFramesBeforeCorrection = state.decodedSampleFrames;
    state.gaplessCorrectedDecodedSampleFrames = correctedFrames;
    state.decodedSampleFrames = correctedFrames;
    state.decodedSampleFramesKind = "estimated_gapless_corrected";
    state.decodedSampleFramesTrust = "authoritative";
    state.decodedSampleFramesSource = "opus_gapless_skip_discard";
    state.frameCountPolicyReason = "opus_gapless_or_packet_timeline_used";
    updateEstimatedDecodedBytes(state);
}

void applyAdtsAacFrameCountPolicy(FrameCountPolicyState& state, const PacketFrameCountScan& scan) {
    if (state.gaplessCorrectionApplied ||
        state.decodedSampleFramesKind == "exact" ||
        !isRawAdtsAac(state) ||
        scan.audioPacketCount <= 0) {
        return;
    }

    std::int64_t candidateFrames = 0;
    std::string candidateSource;
    if (scan.packetDurationSumFrames > 0 &&
        scan.aacFrameCountCandidateFrames > 0 &&
        frameDeltaAbs(scan.packetDurationSumFrames, scan.aacFrameCountCandidateFrames) <= 1024) {
        candidateFrames = scan.packetDurationSumFrames;
        candidateSource = "aac_adts_packet_duration_sum";
    } else if (scan.aacFrameCountCandidateFrames > 0) {
        candidateFrames = scan.aacFrameCountCandidateFrames;
        candidateSource = "aac_adts_packet_count";
    } else if (scan.packetDurationSumFrames > 0) {
        candidateFrames = scan.packetDurationSumFrames;
        candidateSource = "aac_adts_packet_duration_sum";
    } else if (scan.packetPtsSpanFrames > 0) {
        candidateFrames = scan.packetPtsSpanFrames;
        candidateSource = "aac_adts_packet_pts_span";
    }

    if (candidateFrames <= 0) {
        return;
    }

    state.decodedSampleFramesBeforeCorrection = state.decodedSampleFrames;
    state.decodedSampleFrames = candidateFrames;
    state.decodedSampleFramesKind = candidateSource == "aac_adts_packet_duration_sum"
        ? "packet_duration_sum"
        : "packet_derived";
    state.decodedSampleFramesTrust = "authoritative";
    state.decodedSampleFramesSource = candidateSource;
    state.packetFrameCountCandidateUsed = true;
    state.frameCountPolicyReason = "adts_aac_packet_count_or_duration_used";
    updateEstimatedDecodedBytes(state);
}

void applyMpegPsMp2FrameCountPolicy(FrameCountPolicyState& state, const PacketFrameCountScan& scan) {
    if (state.gaplessCorrectionApplied ||
        state.decodedSampleFramesKind == "exact" ||
        !isMpegPsContainer(state.formatName) ||
        !isMp2AudioCodec(state) ||
        scan.mp2FrameCountCandidateFrames <= 0) {
        return;
    }

    state.decodedSampleFramesBeforeCorrection = state.decodedSampleFrames;
    state.decodedSampleFrames = scan.mp2FrameCountCandidateFrames;
    state.decodedSampleFramesKind = "packet_derived";
    state.decodedSampleFramesTrust = "authoritative";
    state.decodedSampleFramesSource = "mpegps_mp2_packet_count";
    state.packetFrameCountCandidateUsed = true;
    state.frameCountPolicyReason = "mpegps_mp2_packet_count_used";
    updateEstimatedDecodedBytes(state);
}

void applyMp4Mp3PacketCountMinusSkipPolicy(FrameCountPolicyState& state, const PacketFrameCountScan& scan) {
    if (state.gaplessCorrectionApplied ||
        state.decodedSampleFramesKind == "exact" ||
        !isMovMp4FamilyContainer(state.formatName) ||
        !isMp3AudioCodec(state) ||
        state.skipSamplesStart <= 0 ||
        state.skipSamplesEnd != 0 ||
        scan.audioPacketCount <= 0 ||
        scan.packetsWithDuration != scan.audioPacketCount ||
        scan.packetsWithTimestamp != scan.audioPacketCount ||
        !scan.packetPtsMonotonic ||
        scan.mp3FrameCountCandidateFrames <= state.skipSamplesStart ||
        !scan.warning.empty()) {
        return;
    }

    const std::int64_t candidateFrames =
        scan.mp3FrameCountCandidateFrames - state.skipSamplesStart;
    const std::int64_t mp3SamplesPerPacket =
        mp3SamplesPerFrameForSampleRate(state.selectedAudio.sampleRate);
    const std::int64_t mp3PacketSanityFrames = 2 * std::max<std::int64_t>(1, mp3SamplesPerPacket);
    if (candidateFrames <= 0 ||
        (state.decodedSampleFrames > 0 &&
         frameDeltaAbs(candidateFrames, state.decodedSampleFrames) > mp3PacketSanityFrames)) {
        return;
    }

    state.decodedSampleFramesBeforeCorrection = state.decodedSampleFrames;
    state.decodedSampleFrames = candidateFrames;
    state.decodedSampleFramesKind = "packet_derived";
    state.decodedSampleFramesTrust = "authoritative";
    state.decodedSampleFramesSource = "mp3_packet_count_minus_skip_start";
    state.packetFrameCountCandidateUsed = true;
    state.frameCountPolicyReason = "mp4_mp3_packet_count_minus_skip_start_used";
    updateEstimatedDecodedBytes(state);
}

void applyWmav2PacketCountMinusDelayPolicy(FrameCountPolicyState& state, const PacketFrameCountScan& scan) {
    if (state.gaplessCorrectionApplied ||
        state.decodedSampleFramesKind == "exact" ||
        !isAsfContainer(state.formatName) ||
        !isWmav2AudioCodec(state) ||
        state.selectedAudio.sampleRate <= 0 ||
        state.selectedAudio.channels <= 0 ||
        scan.audioPacketCount <= 0 ||
        scan.packetPtsSpanWithoutLastDurationFrames <= 0 ||
        scan.packetsWithTimestamp != scan.audioPacketCount ||
        !scan.packetPtsMonotonic ||
        !scan.warning.empty()) {
        return;
    }

    const std::int64_t wmav2DecoderDelayFrames =
        wmav2SamplesPerFrameForSampleRate(state.selectedAudio.sampleRate);
    if (wmav2DecoderDelayFrames <= 0 ||
        scan.wmav2FrameCountCandidateFrames <= wmav2DecoderDelayFrames) {
        return;
    }

    constexpr std::int64_t kAsfPacketTimelineToleranceFrames = 128;
    const std::int64_t candidateFrames =
        scan.wmav2FrameCountCandidateFrames - wmav2DecoderDelayFrames;
    if (candidateFrames <= 0 ||
        frameDeltaAbs(candidateFrames, scan.packetPtsSpanWithoutLastDurationFrames) >
            kAsfPacketTimelineToleranceFrames) {
        return;
    }

    state.decodedSampleFramesBeforeCorrection = state.decodedSampleFrames;
    state.decodedSampleFrames = candidateFrames;
    state.decodedSampleFramesKind = "packet_derived";
    state.decodedSampleFramesTrust = "authoritative";
    state.decodedSampleFramesSource = "wmav2_packet_count_minus_decoder_delay";
    state.packetFrameCountCandidateUsed = true;
    state.frameCountPolicyReason = "wmav2_packet_count_minus_decoder_delay_used";
    updateEstimatedDecodedBytes(state);
}

void applyPacketFrameCountPolicy(FrameCountPolicyState& state, const PacketFrameCountScan& scan) {
    state.packetPtsSpanFrames = scan.packetPtsSpanFrames;
    state.packetDurationSumFrames = scan.packetDurationSumFrames;
    if (!scan.warning.empty()) {
        state.warnings.push_back(scan.warning);
    }

    if (state.gaplessCorrectionApplied ||
        state.decodedSampleFramesKind == "exact" ||
        !isPacketTimelinePolicyContainer(state.formatName) ||
        scan.audioPacketCount <= 0 ||
        scan.packetsWithTimestamp <= 0 ||
        scan.firstPacketPts == AV_NOPTS_VALUE ||
        scan.lastPacketPts == AV_NOPTS_VALUE ||
        scan.packetPtsSpanFrames <= 0 ||
        (scan.lastPacketDuration <= 0 && scan.packetDurationSumFrames <= 0)) {
        return;
    }

    const std::int64_t currentFrames = state.decodedSampleFrames;
    const std::int64_t packetPtsSpan = scan.packetPtsSpanFrames;
    const std::int64_t packetDurationSum = scan.packetDurationSumFrames;
    const std::int64_t currentToPacketDuration =
        packetDurationSum > 0 ? frameDeltaAbs(currentFrames, packetDurationSum) : frameDeltaAbs(currentFrames, packetPtsSpan);
    const std::int64_t packetSpanToDuration =
        packetDurationSum > 0 ? frameDeltaAbs(packetPtsSpan, packetDurationSum) : 0;
    const bool differsMeaningfully =
        currentFrames <= 0 || frameDeltaAbs(packetPtsSpan, currentFrames) >= 1024;
    const bool alignsBetter =
        packetDurationSum <= 0 || packetSpanToDuration <= currentToPacketDuration;
    const bool mpegTsPacketSpanTrusted =
        isMpegTsContainer(state.formatName) &&
        scan.packetPtsMonotonic &&
        scan.packetsWithTimestamp == scan.audioPacketCount &&
        differsMeaningfully;
    const bool packetSpanTrusted =
        differsMeaningfully &&
        alignsBetter &&
        packetSpanToDuration <= 1024;
    if (!mpegTsPacketSpanTrusted && !packetSpanTrusted) {
        return;
    }

    const bool exactPacketSpanTrusted =
        mpegTsPacketSpanTrusted || packetSpanToDuration <= 128;
    const char* reason = mpegTsPacketSpanTrusted
        ? "mpegts_packet_pts_span_used"
        : "packet PTS span is trusted for transport/timeline container and aligns better than duration estimate";

    state.decodedSampleFramesBeforeCorrection = state.decodedSampleFrames;
    state.decodedSampleFrames = packetPtsSpan;
    state.decodedSampleFramesKind = "packet_pts_span";
    state.decodedSampleFramesTrust =
        exactPacketSpanTrusted ? "authoritative" : "aligned_authoritative";
    state.decodedSampleFramesSource = "packet_pts_span";
    state.packetFrameCountCandidateUsed = true;
    state.frameCountPolicyReason = reason;
    updateEstimatedDecodedBytes(state);
}

}  // namespace

bool shouldScanPacketFrameCountCandidates(const FrameCountPolicyState& state) {
    if (state.selectedAudio.sampleRate <= 0 ||
        state.decodedSampleFramesKind == "exact") {
        return false;
    }
    return isPacketTimelinePolicyContainer(state.formatName) ||
        isRawAdtsAac(state) ||
        isOpusAudioCodec(state) ||
        (isMovMp4FamilyContainer(state.formatName) && isMp3AudioCodec(state)) ||
        (isAsfContainer(state.formatName) && isWmav2AudioCodec(state)) ||
        (isMpegPsContainer(state.formatName) && isMp2AudioCodec(state));
}

void recordGaplessSkipSampleScan(
    FrameCountPolicyState& state,
    const GaplessSkipSampleScan& scan) {
    state.decodedSampleFramesBeforeGaplessCorrection = state.decodedSampleFrames;
    state.skipSamplesStart = scan.skipSamplesStart;
    state.skipSamplesEnd = scan.skipSamplesEnd;
    state.skipSamplesTotal = scan.skipSamplesTotal;
    state.gaplessSideDataPacketCount = scan.sideDataPacketCount;
    state.gaplessAudioPacketsScanned = scan.audioPacketsScanned;
    state.gaplessCorrectionSource = scan.source;
    state.gaplessCorrectedDecodedSampleFrames = state.decodedSampleFrames;
    if (!scan.warning.empty()) {
        state.warnings.push_back(scan.warning);
    }
}

void recordOggVorbisTerminalScan(
    FrameCountPolicyState& state,
    const OggVorbisTerminalScan& scan) {
    state.oggVorbisTerminalScanAvailable = scan.attempted;
    state.oggVorbisTerminalScanComplete = scan.scanComplete;
    state.oggVorbisEosObserved = scan.eosObserved;
    state.oggVorbisEosGranuleKnown = scan.selectedAudioEosGranule >= 0;
    state.oggVorbisEosGranuleFrames = scan.selectedAudioEosGranule;
    state.oggVorbisTruncated = scan.truncated;
    state.oggVorbisChainedOrAmbiguous = scan.chainedOrAmbiguous;
    state.oggVorbisTimestampDiscontinuity = scan.timestampDiscontinuity;
    state.oggVorbisSerialNumberCount = scan.serialNumberCount;
    state.oggVorbisVorbisBosCount = scan.vorbisBosCount;
    state.oggVorbisVorbisEosCount = scan.vorbisEosCount;
    state.oggVorbisTerminalScanWarning = scan.warning;
}

void applyGaplessSkipSampleCorrection(
    FrameCountPolicyState& state,
    const GaplessSkipSampleScan& scan) {
    recordGaplessSkipSampleScan(state, scan);
    const bool twoSidedCorrection =
        state.decodedSampleFrames > 0 &&
        state.decodedSampleFramesKind != "exact" &&
        state.skipSamplesStart > 0 &&
        state.skipSamplesEnd > 0 &&
        state.skipSamplesTotal > 0 &&
        state.decodedSampleFrames > state.skipSamplesTotal;
    const bool bareMp3OneSidedStartSkipCorrection =
        state.decodedSampleFrames > 0 &&
        state.decodedSampleFramesKind != "exact" &&
        isBareMp3Container(state.formatName) &&
        isMp3AudioCodec(state) &&
        state.skipSamplesStart > 0 &&
        state.skipSamplesEnd == 0 &&
        state.decodedSampleFrames > state.skipSamplesStart;
    if (!twoSidedCorrection && !bareMp3OneSidedStartSkipCorrection) {
        return;
    }

    const std::int64_t correctionFrames =
        twoSidedCorrection ? state.skipSamplesTotal : state.skipSamplesStart;
    const std::int64_t correctedFrames = state.decodedSampleFrames - correctionFrames;
    if (correctedFrames <= 0) {
        return;
    }

    state.gaplessCorrectionApplied = true;
    state.decodedSampleFramesBeforeCorrection = state.decodedSampleFrames;
    state.gaplessCorrectedDecodedSampleFrames = correctedFrames;
    state.decodedSampleFrames = correctedFrames;
    state.decodedSampleFramesKind = "estimated_gapless_corrected";
    state.decodedSampleFramesTrust = "authoritative";
    state.decodedSampleFramesSource = "mp3_gapless_skip_samples";
    state.frameCountPolicyReason = bareMp3OneSidedStartSkipCorrection
        ? "bare_mp3_one_sided_start_skip_applied"
        : "packet skip/discard side data corrected the duration estimate";
    updateEstimatedDecodedBytes(state);
}

void applyOggVorbisExactExtentPolicy(FrameCountPolicyState& state) {
    applyOggVorbisExactExtentPolicyInternal(state);
}

void applyPacketFrameCountPolicies(
    FrameCountPolicyState& state,
    const PacketFrameCountScan& scan) {
    state.packetPtsSpanFrames = scan.packetPtsSpanFrames;
    state.packetDurationSumFrames = scan.packetDurationSumFrames;
    applyAdtsAacFrameCountPolicy(state, scan);
    applyOpusFrameCountPolicy(state, scan);
    applyMp4Mp3PacketCountMinusSkipPolicy(state, scan);
    applyPacketFrameCountPolicy(state, scan);
    applyMpegPsMp2FrameCountPolicy(state, scan);
    applyWmav2PacketCountMinusDelayPolicy(state, scan);
}

void finalizeFrameCountTrustPolicy(
    FrameCountPolicyState& state,
    bool selectedCodecKnown,
    bool selectedCodecIsPcm) {
    if (state.decodedSampleFramesTrust == "authoritative" ||
        state.decodedSampleFramesTrust == "aligned_authoritative") {
        return;
    }

    if (state.decodedSampleFrames <= 0) {
        state.decodedSampleFramesTrust = "unknown";
        state.decodedSampleFramesSource = "unknown";
        state.frameCountPolicyReason = "no decoded sample frame estimate is available";
        return;
    }

    if (state.decodedSampleFramesKind == "exact") {
        state.decodedSampleFramesTrust = "authoritative";
        state.decodedSampleFramesSource = "exact_pcm_stream_duration";
        state.frameCountPolicyReason = "exact frame count accepted";
        return;
    }

    if (selectedCodecKnown && !selectedCodecIsPcm) {
        state.decodedSampleFramesTrust = "unsafe_estimated";
        if (state.decodedSampleFramesSource == "unknown") {
            state.decodedSampleFramesSource = "duration_estimate";
        }
        state.frameCountPolicyReason =
            "compressed stream uses duration-derived frame estimate with no trusted correction candidate";
        return;
    }

    state.decodedSampleFramesTrust = "unknown";
    state.frameCountPolicyReason = "frame count trust could not be established";
}

void traceFrameCountPolicyDecisionIfEnabled(const FrameCountPolicyState& state) {
    if (!auditEnabled()) {
        return;
    }

    std::ostringstream out;
    out << "[AVEMEDIABRIDGE_FRAME_COUNT_POLICY]"
        << " container=" << state.formatName
        << " codec=" << state.selectedAudio.codecName
        << " streamFrames=" << state.streamDurationFrames
        << " formatFrames=" << state.formatDurationFrames
        << " eosGranuleKnown=" << boolText(state.oggVorbisEosGranuleKnown)
        << " eosGranuleFrames=" << state.oggVorbisEosGranuleFrames
        << " packetTerminalKnown=" << boolText(state.oggVorbisEosGranuleKnown || state.packetPtsSpanFrames > 0)
        << " packetTerminalFrames="
        << (state.packetPtsSpanFrames > 0 ? state.packetPtsSpanFrames : state.oggVorbisEosGranuleFrames)
        << " packetScanComplete="
        << boolText(state.packetPtsSpanFrames > 0 || state.oggVorbisTerminalScanComplete)
        << " truncated=" << boolText(state.oggVorbisTruncated)
        << " chained=" << boolText(state.oggVorbisChainedOrAmbiguous)
        << " skipStart=" << state.skipSamplesStart
        << " skipEnd=" << state.skipSamplesEnd
        << " decisionFrames=" << state.decodedSampleFrames
        << " decisionKind=" << state.decodedSampleFramesKind
        << " decisionTrust=" << state.decodedSampleFramesTrust
        << " decisionSource=" << state.decodedSampleFramesSource
        << " decisionReason=" << state.frameCountPolicyReason;
    std::cout << out.str() << '\n';
}

}  // namespace AveMediaBridge::Probe
