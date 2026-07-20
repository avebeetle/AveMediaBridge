#include "FrameCountPolicy.hpp"

#include <algorithm>
#include <cstdint>

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

ExactPacketPresentationBudget resolveExactPacketPresentationBudget(
    const ExactPacketPresentationEvidence& evidence) {
    ExactPacketPresentationBudget result;
    if (!evidence.packetScanReachedEof || evidence.packetScanReadError) {
        result.rejectionReason = "packet_scan_incomplete";
        return result;
    }
    if (!evidence.physicalFrameCountKnown || !evidence.physicalFrameCountExact) {
        result.rejectionReason = "physical_frame_count_not_exact";
        return result;
    }
    if (!evidence.initialSkipKnown || !evidence.initialSkipAuthoritative) {
        result.rejectionReason = "initial_skip_not_authoritative";
        return result;
    }
    if (!evidence.terminalDiscardKnown || !evidence.terminalDiscardAuthoritative) {
        result.rejectionReason = "terminal_discard_not_authoritative";
        return result;
    }
    if (evidence.conflictingGaplessEvidence) {
        result.rejectionReason = "conflicting_gapless_evidence";
        return result;
    }
    if (evidence.physicalFrames <= 0 ||
        evidence.initialSkipFrames < 0 ||
        evidence.terminalDiscardFrames < 0) {
        result.rejectionReason = "invalid_frame_count";
        return result;
    }
    if (evidence.initialSkipFrames > evidence.physicalFrames) {
        result.rejectionReason = "initial_skip_exceeds_physical_frames";
        return result;
    }

    const std::int64_t afterInitialSkip =
        evidence.physicalFrames - evidence.initialSkipFrames;
    if (evidence.terminalDiscardFrames > afterInitialSkip) {
        result.rejectionReason = "terminal_discard_exceeds_remaining_frames";
        return result;
    }

    result.presentationFrames = afterInitialSkip - evidence.terminalDiscardFrames;
    if (result.presentationFrames <= 0) {
        result.presentationFrames = 0;
        result.rejectionReason = "empty_presentation";
        return result;
    }
    if (evidence.independentPresentationFramesKnown &&
        (evidence.independentPresentationFrames <= 0 ||
         evidence.independentPresentationFrames != result.presentationFrames)) {
        result.presentationFrames = 0;
        result.rejectionReason = "independent_presentation_conflict";
        return result;
    }
    result.accepted = true;
    result.rejectionReason = "accepted";
    return result;
}

ExactPacketPresentationEvidence makeExactPacketPresentationEvidence(
    const AudioPresentationEvidenceScan& scan) {
    ExactPacketPresentationEvidence evidence;
    const PacketFrameCountScan& packet = scan.packetTiming;
    const GaplessSkipSampleScan& gapless = scan.gapless;
    const bool completeTraversal =
        packet.reachedEof &&
        gapless.reachedEof &&
        !packet.readError &&
        !gapless.readError &&
        packet.warning.empty() &&
        gapless.warning.empty() &&
        packet.audioPacketCount > 0 &&
        gapless.audioPacketsScanned == packet.audioPacketCount;
    const bool initialSkipAuthorityObserved =
        completeTraversal &&
        (gapless.sideDataPacketCount > 0 ||
         gapless.streamInitialPadding > 0 ||
         gapless.streamTrailingPadding > 0);

    // A short terminal packet is authority only when every duration maps
    // exactly to samples and independently matches the packet PTS span.
    const bool packetDurationTimelineExact =
        completeTraversal &&
        packet.packetsWithSampleExactDuration == packet.audioPacketCount &&
        packet.sampleExactPacketDurationSumFrames > 0 &&
        packet.packetPtsSpanFrames == packet.sampleExactPacketDurationSumFrames;
    const bool terminalDiscardAuthorityObserved =
        completeTraversal &&
        (gapless.packetSkipSamplesEnd > 0 ||
         gapless.streamTrailingPadding > 0 ||
         gapless.streamInitialPadding > 0 ||
         packetDurationTimelineExact);

    evidence.packetScanReachedEof = completeTraversal;
    evidence.packetScanReadError = packet.readError || gapless.readError;
    evidence.physicalFrameCountKnown = packet.codecFrameCountKnown;
    evidence.physicalFrameCountExact = packet.codecFrameCountExact;
    evidence.physicalFrames = packet.codecFrameCountFrames;
    evidence.initialSkipKnown = initialSkipAuthorityObserved;
    evidence.initialSkipAuthoritative = initialSkipAuthorityObserved;
    evidence.initialSkipFrames = gapless.skipSamplesStart;
    evidence.terminalDiscardKnown = terminalDiscardAuthorityObserved;
    evidence.terminalDiscardAuthoritative = terminalDiscardAuthorityObserved;
    evidence.terminalDiscardFrames = gapless.skipSamplesEnd;

    if (packetDurationTimelineExact && packet.codecFrameCountExact) {
        if (packet.sampleExactPacketDurationSumFrames > packet.codecFrameCountFrames) {
            evidence.conflictingGaplessEvidence = true;
        } else {
            const std::int64_t durationImpliedTerminalDiscard =
                packet.codecFrameCountFrames - packet.sampleExactPacketDurationSumFrames;
            if (evidence.terminalDiscardFrames > 0 &&
                durationImpliedTerminalDiscard > 0 &&
                evidence.terminalDiscardFrames != durationImpliedTerminalDiscard) {
                evidence.conflictingGaplessEvidence = true;
            } else {
                evidence.terminalDiscardFrames = (std::max)(
                    evidence.terminalDiscardFrames,
                    durationImpliedTerminalDiscard);
            }
        }
    }
    return evidence;
}

bool applyExactPacketPresentationBudget(
    FrameCountPolicyState& state,
    const AudioPresentationEvidenceScan& scan,
    std::int64_t independentPresentationFrames) {
    if (state.decodedSampleFramesKind == "exact") {
        return false;
    }

    ExactPacketPresentationEvidence evidence =
        makeExactPacketPresentationEvidence(scan);
    if (independentPresentationFrames > 0) {
        evidence.independentPresentationFramesKnown = true;
        evidence.independentPresentationFrames = independentPresentationFrames;
    }
    const ExactPacketPresentationBudget budget =
        resolveExactPacketPresentationBudget(evidence);
    if (!budget.accepted) {
        return false;
    }

    state.decodedSampleFramesBeforeCorrection = state.decodedSampleFrames;
    state.decodedSampleFramesBeforeGaplessCorrection = evidence.physicalFrames;
    state.decodedSampleFrames = budget.presentationFrames;
    state.decodedSampleFramesKind = "exact";
    state.decodedSampleFramesTrust = "authoritative";
    state.decodedSampleFramesSource = "exact_packet_presentation";
    state.packetFrameCountCandidateUsed = true;
    state.packetPtsSpanFrames = scan.packetTiming.packetPtsSpanFrames;
    state.packetDurationSumFrames = scan.packetTiming.packetDurationSumFrames;
    state.skipSamplesStart = evidence.initialSkipFrames;
    state.skipSamplesEnd = evidence.terminalDiscardFrames;
    state.skipSamplesTotal = evidence.initialSkipFrames + evidence.terminalDiscardFrames;
    state.gaplessCorrectedDecodedSampleFrames = budget.presentationFrames;
    state.gaplessCorrectionApplied = state.skipSamplesTotal > 0;
    state.gaplessCorrectionSource = scan.gapless.source;
    state.gaplessSideDataPacketCount = scan.gapless.sideDataPacketCount;
    state.gaplessAudioPacketsScanned = scan.gapless.audioPacketsScanned;
    state.frameCountPolicyReason =
        "complete codec-frame and gapless evidence proved exact presentation frames";
    updateEstimatedDecodedBytes(state);
    return true;
}

bool shouldEvaluateExactPacketPresentationAfterScan(
    const FrameCountPolicyState& state,
    bool allowed,
    bool packetScanPerformed,
    bool gaplessScanPerformed) {
    return allowed &&
        state.decodedSampleFramesKind != "exact" &&
        packetScanPerformed &&
        gaplessScanPerformed;
}

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

}  // namespace AveMediaBridge::Probe
