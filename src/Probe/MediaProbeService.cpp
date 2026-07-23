#include "MediaProbeService.hpp"

#include "AdtsAacSequentialPresentation.hpp"
#include "Ac3Eac3SequentialPresentation.hpp"
#include "FrameCountPolicy.hpp"
#include "MatroskaAacSequentialPresentation.hpp"
#include "Mp4Mp3SampleEditTablePresentation.hpp"
#include "Mp3HeaderPresentation.hpp"
#include "NutBoundedTailAuthority.hpp"
#include "OggOpusSequentialPresentation.hpp"
#include "PacketScan.hpp"
#include "PresentationBudgetPolicy.hpp"
#include "../Core/MediaBridgeError.hpp"
#include "../Ffmpeg/FfmpegDeleters.hpp"
#include "../Ffmpeg/FfmpegStreamSelection.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>

namespace AveMediaBridge::Probe {
namespace {

namespace Ffmpeg = AveMediaBridge::Ffmpeg;
using AveMediaBridge::ffErrorString;

constexpr std::int64_t kFastProbeSizeBytes = 4 * 1024 * 1024;
constexpr std::int64_t kFastProbeAnalyzeDurationUs = 3 * AV_TIME_BASE;

std::string mediaTypeName(AVMediaType type) {
    const char* name = av_get_media_type_string(type);
    return name ? std::string(name) : std::string("unknown");
}

std::string describeChannelLayout(const AVChannelLayout& layout) {
    if (layout.nb_channels <= 0 || !av_channel_layout_check(&layout)) {
        return {};
    }

    char buffer[128] = {};
    if (av_channel_layout_describe(&layout, buffer, sizeof(buffer)) <= 0) {
        return {};
    }
    return std::string(buffer);
}

bool isPcmCodec(AVCodecID codecId) {
    switch (codecId) {
        case AV_CODEC_ID_PCM_S16LE:
        case AV_CODEC_ID_PCM_S16BE:
        case AV_CODEC_ID_PCM_U16LE:
        case AV_CODEC_ID_PCM_U16BE:
        case AV_CODEC_ID_PCM_S8:
        case AV_CODEC_ID_PCM_U8:
        case AV_CODEC_ID_PCM_MULAW:
        case AV_CODEC_ID_PCM_ALAW:
        case AV_CODEC_ID_PCM_S32LE:
        case AV_CODEC_ID_PCM_S32BE:
        case AV_CODEC_ID_PCM_U32LE:
        case AV_CODEC_ID_PCM_U32BE:
        case AV_CODEC_ID_PCM_S24LE:
        case AV_CODEC_ID_PCM_S24BE:
        case AV_CODEC_ID_PCM_U24LE:
        case AV_CODEC_ID_PCM_U24BE:
        case AV_CODEC_ID_PCM_S24DAUD:
        case AV_CODEC_ID_PCM_ZORK:
        case AV_CODEC_ID_PCM_S16LE_PLANAR:
        case AV_CODEC_ID_PCM_DVD:
        case AV_CODEC_ID_PCM_F32BE:
        case AV_CODEC_ID_PCM_F32LE:
        case AV_CODEC_ID_PCM_F64BE:
        case AV_CODEC_ID_PCM_F64LE:
        case AV_CODEC_ID_PCM_BLURAY:
        case AV_CODEC_ID_PCM_LXF:
        case AV_CODEC_ID_PCM_S8_PLANAR:
        case AV_CODEC_ID_PCM_S24LE_PLANAR:
        case AV_CODEC_ID_PCM_S32LE_PLANAR:
        case AV_CODEC_ID_PCM_S16BE_PLANAR:
        case AV_CODEC_ID_PCM_S64LE:
        case AV_CODEC_ID_PCM_S64BE:
        case AV_CODEC_ID_PCM_F16LE:
        case AV_CODEC_ID_PCM_F24LE:
        case AV_CODEC_ID_PCM_VIDC:
        case AV_CODEC_ID_PCM_SGA:
            return true;
        default:
            return false;
    }
}

double secondsFromStreamDuration(const AVStream* stream) {
    if (!stream || stream->duration == AV_NOPTS_VALUE || stream->duration <= 0 ||
        stream->time_base.num <= 0 || stream->time_base.den <= 0) {
        return 0.0;
    }

    return static_cast<double>(stream->duration) * av_q2d(stream->time_base);
}

std::int64_t exactFramesFromSampleDomainStreamDuration(const AVStream* stream) {
    const AVCodecParameters* codecpar = stream ? stream->codecpar : nullptr;
    if (!stream || !codecpar || codecpar->sample_rate <= 0 ||
        stream->duration == AV_NOPTS_VALUE || stream->duration <= 0 ||
        stream->time_base.num <= 0 || stream->time_base.den <= 0) {
        return 0;
    }

    const AVRational sampleTimeBase{1, codecpar->sample_rate};
    if (av_cmp_q(stream->time_base, sampleTimeBase) != 0) {
        return 0;
    }
    const std::int64_t frames =
        av_rescale_q(stream->duration, stream->time_base, sampleTimeBase);
    return frames > 0 &&
            av_compare_ts(stream->duration, stream->time_base, frames, sampleTimeBase) == 0
        ? frames
        : 0;
}

void updateEstimatedDecodedBytes(FastProbeJsonDocument& document) {
    if (document.decodedSampleFrames > 0 && document.selectedAudio.channels > 0) {
        document.estimatedDecodedBytes =
            document.decodedSampleFrames *
            static_cast<std::int64_t>(document.selectedAudio.channels) *
            static_cast<std::int64_t>(sizeof(float));
        document.estimatedDecodedBytesKind = document.decodedSampleFramesKind;
    }
}

void fillFastSourceInfo(FastProbeResult& result, const AVFormatContext* formatContext) {
    if (!formatContext) {
        return;
    }

    result.document.streamCount = static_cast<int>(formatContext->nb_streams);
    result.document.probeScore = formatContext->probe_score;
    if (formatContext->iformat) {
        result.document.formatName = formatContext->iformat->name ? formatContext->iformat->name : "";
        result.document.formatLongName =
            formatContext->iformat->long_name ? formatContext->iformat->long_name : "";
        result.document.containerFormat =
            !result.document.formatLongName.empty()
                ? result.document.formatLongName
                : result.document.formatName;
    }
}

void fillFastStreamDetails(FastProbeResult& result, const AVFormatContext* formatContext) {
    if (!formatContext) {
        return;
    }

    result.document.streams.reserve(formatContext->nb_streams);
    for (unsigned int i = 0; i < formatContext->nb_streams; ++i) {
        const AVStream* stream = formatContext->streams[i];
        if (stream && stream->codecpar && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ++result.document.audioStreamCount;
        }
        result.document.streams.push_back(makeStreamSummary(static_cast<int>(i), stream));
    }
}

void fillFastSelectedAudio(
    FastProbeResult& result,
    const AVStream* stream,
    const AVCodec* decoder) {
    if (!stream || !stream->codecpar) {
        return;
    }

    const AVCodecParameters* codecpar = stream->codecpar;
    result.document.hasAudio = true;
    result.document.selectedAudio.index = static_cast<int>(stream->index);
    result.document.selectedAudio.codecName = avcodec_get_name(codecpar->codec_id);
    result.document.selectedAudio.codecId = static_cast<int>(codecpar->codec_id);
    result.document.selectedAudio.decoderName = decoder && decoder->name ? decoder->name : "";
    result.document.selectedAudio.sampleRate = codecpar->sample_rate;
    result.document.selectedAudio.channels = codecpar->ch_layout.nb_channels;
    result.document.selectedAudio.bitRate = codecpar->bit_rate;
    result.document.selectedAudio.timeBase = rationalToString(stream->time_base);
    result.document.channelLayout = describeChannelLayout(codecpar->ch_layout);
}

void estimateFastDurationAndFrames(
    FastProbeResult& result,
    const AVFormatContext* formatContext,
    const AVStream* audioStream) {
    FastProbeJsonDocument& document = result.document;
    const AVCodecParameters* codecpar = audioStream && audioStream->codecpar ? audioStream->codecpar : nullptr;
    const double streamSeconds = secondsFromStreamDuration(audioStream);
    result.totalPresentation = makeStreamTotalPresentationEvidence(formatContext, audioStream);
    const bool sampleExactTotal =
        result.totalPresentation.trust == PresentationTotalTrust::SampleExact;
    const std::int64_t exactPresentationFrames =
        sampleExactTotal &&
            result.totalPresentation.frames <=
                static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
        ? static_cast<std::int64_t>(result.totalPresentation.frames)
        : 0;

    if (streamSeconds > 0.0) {
        document.durationSec = streamSeconds;
        document.durationEstimationMethod = "from_stream";
        document.durationKind = exactPresentationFrames > 0 ? "exact" : "estimated";
    } else if (formatContext && formatContext->duration != AV_NOPTS_VALUE && formatContext->duration > 0) {
        document.durationSec = static_cast<double>(formatContext->duration) / static_cast<double>(AV_TIME_BASE);
        document.durationEstimationMethod = "from_pts";
        document.durationKind = "estimated";
    } else if (formatContext) {
        const std::int64_t bitRate =
            codecpar && codecpar->bit_rate > 0 ? codecpar->bit_rate : formatContext->bit_rate;
        const std::int64_t byteSize = formatContext->pb ? avio_size(formatContext->pb) : -1;
        if (bitRate > 0 && byteSize > 0) {
            document.durationSec = (static_cast<double>(byteSize) * 8.0) / static_cast<double>(bitRate);
            document.durationEstimationMethod = "from_bitrate";
            document.durationKind = "estimated";
        }
    }

    const int sampleRate = codecpar ? codecpar->sample_rate : 0;
    if (exactPresentationFrames > 0) {
        document.decodedSampleFrames = exactPresentationFrames;
        document.decodedSampleFramesKind = "exact";
        document.decodedSampleFramesTrust = "authoritative";
        document.decodedSampleFramesSource =
            presentationTotalSourceName(result.totalPresentation.source);
        document.frameCountPolicyReason =
            result.totalPresentation.source == PresentationTotalSource::OggEosGranule
                ? "Ogg EOS granule provides the half-open PCM presentation end"
                : result.totalPresentation.source ==
                        PresentationTotalSource::FlacStreamInfoTotalSamples
                    ? "FLAC STREAMINFO total_samples provides the half-open PCM presentation end"
                    : "stream duration is authoritative in the native sample domain";
    } else if (document.durationSec > 0.0 && sampleRate > 0 && std::isfinite(document.durationSec)) {
        document.decodedSampleFrames =
            static_cast<std::int64_t>(std::llround(document.durationSec * static_cast<double>(sampleRate)));
        document.decodedSampleFramesKind = document.decodedSampleFrames > 0 ? "estimated" : "unknown";
        if (document.decodedSampleFrames > 0) {
            if (document.durationEstimationMethod == "from_stream") {
                document.decodedSampleFramesSource = "stream_duration_estimate";
            } else if (document.durationEstimationMethod == "from_pts") {
                document.decodedSampleFramesSource = "format_duration_estimate";
            } else {
                document.decodedSampleFramesSource = "duration_estimate";
            }
            document.frameCountPolicyReason = "duration-derived compressed frame count is estimated";
        }
    }

    updateEstimatedDecodedBytes(document);
}

void applyNutBoundedTailAuthority(
    FastProbeResult& result,
    const std::string& path,
    const AVFormatContext* formatContext,
    const AVStream* audioStream) {
    const bool strongerExactAuthority =
        result.totalPresentation.trust == PresentationTotalTrust::SampleExact;
    if (!shouldProbeNutBoundedTailAuthority(
            path, formatContext, audioStream, strongerExactAuthority)) {
        return;
    }

    result.nutBoundedTail = probeNutBoundedTailAuthority(
        path, static_cast<int>(audioStream->index));
    FastProbeJsonDocument& document = result.document;
    document.nutBoundedTailStatus =
        nutBoundedTailStatusName(result.nutBoundedTail.status);
    document.nutBoundedTailReason = result.nutBoundedTail.reason;
    document.nutBoundedTailBudgetBytes =
        result.nutBoundedTail.hardReadBudgetBytes;
    document.nutBoundedTailActualReadBytes =
        result.nutBoundedTail.totalActualReadBytes;
    document.nutBoundedTailMaximumBudgetOverrunBytes =
        result.nutBoundedTail.maximumBudgetOverrunBytes;
    document.nutBoundedTailPacketsObserved =
        result.nutBoundedTail.targetPacketsObserved;
    document.nutBoundedTailReachedEof = result.nutBoundedTail.reachedPhysicalEof;
    if (!result.nutBoundedTail.exact()) {
        return;
    }

    TotalPresentationEvidence evidence;
    evidence.frames = result.nutBoundedTail.presentationFrames;
    evidence.trust = PresentationTotalTrust::SampleExact;
    evidence.source = PresentationTotalSource::NutBoundedTailSelectedStreamEnd;
    evidence.domain = PresentationSampleDomain::NativeStreamSamples;
    evidence.sampleRate = result.nutBoundedTail.sampleRate;
    evidence.exactRescale = true;
    evidence.validation = PresentationTotalValidation::SelfContainedMetadata;
    result.totalPresentation = evidence;

    if (evidence.frames >
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        return;
    }
    document.decodedSampleFrames = static_cast<std::int64_t>(evidence.frames);
    document.decodedSampleFramesKind = "exact";
    document.decodedSampleFramesTrust = "authoritative";
    document.decodedSampleFramesSource = presentationTotalSourceName(evidence.source);
    document.frameCountPolicyReason =
        "bounded NUT tail proves the selected-stream half-open PCM end";
    document.durationSec = static_cast<double>(evidence.frames) /
        static_cast<double>(evidence.sampleRate);
    document.durationKind = "exact";
    document.durationEstimationMethod = "from_nut_bounded_tail";
    updateEstimatedDecodedBytes(document);
}

void applyMp3HeaderPresentationAuthority(
    FastProbeResult& result,
    const std::string& path,
    const AVFormatContext* formatContext,
    const AVStream* audioStream) {
    const bool strongerExactAuthority =
        result.totalPresentation.trust == PresentationTotalTrust::SampleExact;
    if (!shouldProbeMp3HeaderPresentation(
            formatContext, audioStream, strongerExactAuthority)) {
        return;
    }

    result.mp3HeaderPresentation = probeMp3HeaderPresentation(path);
    FastProbeJsonDocument& document = result.document;
    const Mp3HeaderPresentationResult& header = result.mp3HeaderPresentation;
    document.mp3HeaderPresentationStatus =
        mp3HeaderPresentationStatusName(header.status);
    document.mp3HeaderPresentationReason = header.reason;
    document.mp3HeaderType = mp3HeaderTypeName(header.headerType);
    document.mp3HeaderEncoderProfile = header.encoderProfile;
    document.mp3HeaderBudgetBytes = header.hardReadBudgetBytes;
    document.mp3HeaderActualReadBytes = header.totalActualReadBytes;
    document.mp3HeaderUniqueBytesRead = header.uniqueBytesRead;
    document.mp3HeaderMaximumBudgetOverrunBytes =
        header.maximumBudgetOverrunBytes;
    document.mp3HeaderReadCalls = header.readCallCount;
    document.mp3HeaderSeekCalls = header.seekCallCount;
    document.mp3HeaderMaximumOffsetReached = header.maximumOffsetReached;
    document.mp3HeaderPhysicalFrameCount = header.physicalFrameCount;
    document.mp3HeaderSamplesPerFrame = header.samplesPerFrame;
    document.mp3HeaderPhysicalSampleTotal = header.physicalSampleTotal;
    document.mp3HeaderInitialPresentationSkip = header.initialPresentationSkip;
    document.mp3HeaderTerminalPresentationPadding =
        header.terminalPresentationPadding;
    document.mp3HeaderPresentationFrames = header.presentationFrames;
    if (!header.exact()) {
        return;
    }
    if (!mp3HeaderPresentationMatchesStream(header, audioStream)) {
        result.mp3HeaderPresentation.status =
            Mp3HeaderPresentationStatus::Conflict;
        result.mp3HeaderPresentation.reason =
            "validated_header_conflicts_with_selected_stream_parameters";
        document.mp3HeaderPresentationStatus = "conflict";
        document.mp3HeaderPresentationReason = result.mp3HeaderPresentation.reason;
        return;
    }

    TotalPresentationEvidence evidence =
        makeMp3HeaderTotalPresentationEvidence(header);
    evidence = reconcileTotalPresentationEvidence(evidence, result.totalPresentation);
    if (evidence.conflict) {
        result.mp3HeaderPresentation.status =
            Mp3HeaderPresentationStatus::Conflict;
        result.mp3HeaderPresentation.reason =
            "validated_header_conflicts_with_independent_exact_total";
        document.mp3HeaderPresentationStatus = "conflict";
        document.mp3HeaderPresentationReason = result.mp3HeaderPresentation.reason;
        return;
    }
    if (evidence.frames >
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        return;
    }

    result.totalPresentation = evidence;
    document.decodedSampleFrames = static_cast<std::int64_t>(evidence.frames);
    document.decodedSampleFramesKind = "exact";
    document.decodedSampleFramesTrust = "authoritative";
    document.decodedSampleFramesSource = presentationTotalSourceName(evidence.source);
    document.frameCountPolicyReason =
        "validated standalone MP3 header provides exact gapless presentation";
    document.durationSec = static_cast<double>(evidence.frames) /
        static_cast<double>(evidence.sampleRate);
    document.durationKind = "exact";
    document.durationEstimationMethod = "from_mp3_validated_header";
    document.mp3HeaderFullScanSkipped = true;
    updateEstimatedDecodedBytes(document);
}

void copyMp4Mp3SampleTableDiagnostics(
    FastProbeJsonDocument& document,
    const Mp4Mp3SampleEditTablePresentationResult& table) {
    document.mp4Mp3SampleTableStatus = mp4Mp3SampleTableStatusName(table.status);
    document.mp4Mp3SampleTableReason = mp4Mp3SampleTableReasonName(table.reason);
    document.mp4Mp3SampleTableSelectedTrackId = table.selectedTrackId;
    document.mp4Mp3SampleTableSampleRate = table.sampleRate;
    document.mp4Mp3SampleTableChannels = table.channels;
    document.mp4Mp3SampleTableSelectedSamples = table.selectedSampleCount;
    document.mp4Mp3SampleTableSamplesPerMp3Frame = table.samplesPerMp3Frame;
    document.mp4Mp3SampleTablePhysicalFrames = table.physicalFrames;
    document.mp4Mp3SampleTableInitialSkip = table.initialSkipFrames;
    document.mp4Mp3SampleTableTerminalDiscard = table.terminalDiscardFrames;
    document.mp4Mp3SampleTableEditedMediaEnd = table.editedMediaEnd;
    document.mp4Mp3SampleTablePresentationFrames = table.presentationFrames;
    document.mp4Mp3SampleTableMovieTimescale = table.movieTimescale;
    document.mp4Mp3SampleTableMediaTimescale = table.mediaTimescale;
    document.mp4Mp3SampleTableEditMediaStart = table.editMediaStart;
    document.mp4Mp3SampleTableEditPresentationFrames = table.editPresentationFrames;
    document.mp4Mp3SampleTableFileSizeBytes = table.fileSizeBytes;
    document.mp4Mp3SampleTableFileIndex = table.fileIndex;
    document.mp4Mp3SampleTableLastWriteTime100ns = table.lastWriteTime100ns;
    document.mp4Mp3SampleTableVolumeSerialNumber = table.volumeSerialNumber;
    document.mp4Mp3SampleTableBudgetBytes = table.hardReadBudgetBytes;
    document.mp4Mp3SampleTableBytesReturned = table.bytesReturned;
    document.mp4Mp3SampleTableUniqueBytes = table.uniqueBytes;
    document.mp4Mp3SampleTableDuplicateBytes = table.duplicateBytes;
    document.mp4Mp3SampleTableMaximumBudgetOverrunBytes =
        table.maximumBudgetOverrunBytes;
    document.mp4Mp3SampleTableReadCalls = table.readCalls;
    document.mp4Mp3SampleTableSeekCalls = table.seekCalls;
    document.mp4Mp3SampleTableMaximumOffsetReached = table.maximumOffsetReached;
    document.mp4Mp3SampleTableScanDurationUs = table.scanDurationUs;
    document.mp4Mp3SampleTableMaximumWorkingBufferBytes =
        table.maximumWorkingBufferBytes;
    document.mp4Mp3SampleTableBoxesParsed = table.boxesParsed;
    document.mp4Mp3SampleTableEntriesParsed = table.tableEntriesParsed;
    document.mp4Mp3SampleTableSelectedChunks = table.selectedChunks;
    document.mp4Mp3SampleTableMoovOffset = table.moovOffset;
    document.mp4Mp3SampleTableMoovSize = table.moovSize;
    document.mp4Mp3SampleTableMoovAtHead = table.moovAtHead;
    document.mp4Mp3SampleTableMoovAtTail = table.moovAtTail;
    document.mp4Mp3SampleTableSampleInventoryValid = table.sampleInventoryValid;
    document.mp4Mp3SampleTableChunkMappingValid = table.chunkMappingValid;
    document.mp4Mp3SampleTableChunkRangesInsideMdat = table.chunkRangesInsideMdat;
    document.mp4Mp3SampleTableEditListValid = table.editListValid;
    document.mp4Mp3SampleTableMp3ProfileValid = table.mp3ProfileValid;
    document.mp4Mp3SampleTableCheckedArithmeticValid = table.checkedArithmeticValid;
}

void applyMp4Mp3SampleEditTablePresentationAuthority(
    FastProbeResult& result,
    const std::string& path,
    const AVFormatContext* formatContext,
    const AVStream* audioStream) {
    const bool strongerExactAuthority =
        result.totalPresentation.trust == PresentationTotalTrust::SampleExact;
    const Mp4Mp3SampleTableEligibility eligibility =
        evaluateMp4Mp3SampleTableEligibility(
            path, formatContext, audioStream, strongerExactAuthority);
    FastProbeJsonDocument& document = result.document;
    document.mp4Mp3SampleTableEligible = eligibility.eligible;
    document.mp4Mp3SampleTableReason =
        mp4Mp3SampleTableReasonName(eligibility.reason);
    if (!eligibility.eligible) {
        return;
    }

    document.mp4Mp3SampleTableEntered = true;
    result.mp4Mp3SampleEditTablePresentation =
        probeMp4Mp3SampleEditTablePresentation(path, eligibility);
    copyMp4Mp3SampleTableDiagnostics(
        document, result.mp4Mp3SampleEditTablePresentation);
    if (!result.mp4Mp3SampleEditTablePresentation.exact()) {
        return;
    }
    if (!mp4Mp3SampleTableMatchesStream(
            result.mp4Mp3SampleEditTablePresentation, audioStream)) {
        result.mp4Mp3SampleEditTablePresentation.status =
            Mp4Mp3SampleTableStatus::Conflict;
        result.mp4Mp3SampleEditTablePresentation.reason =
            Mp4Mp3SampleTableReason::IndependentExactAuthorityConflict;
        copyMp4Mp3SampleTableDiagnostics(
            document, result.mp4Mp3SampleEditTablePresentation);
        return;
    }

    TotalPresentationEvidence evidence =
        makeMp4Mp3SampleTableTotalPresentationEvidence(
            result.mp4Mp3SampleEditTablePresentation);
    evidence = reconcileTotalPresentationEvidence(evidence, result.totalPresentation);
    const auto& table = result.mp4Mp3SampleEditTablePresentation;
    constexpr std::uint64_t maxDiagnosticFrames =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    if (evidence.conflict ||
        evidence.frames > maxDiagnosticFrames ||
        table.physicalFrames > maxDiagnosticFrames ||
        table.initialSkipFrames > maxDiagnosticFrames ||
        table.terminalDiscardFrames > maxDiagnosticFrames ||
        table.initialSkipFrames > maxDiagnosticFrames - table.terminalDiscardFrames) {
        result.mp4Mp3SampleEditTablePresentation.status =
            Mp4Mp3SampleTableStatus::Conflict;
        result.mp4Mp3SampleEditTablePresentation.reason =
            Mp4Mp3SampleTableReason::IndependentExactAuthorityConflict;
        copyMp4Mp3SampleTableDiagnostics(
            document, result.mp4Mp3SampleEditTablePresentation);
        return;
    }

    result.totalPresentation = evidence;
    document.decodedSampleFrames = static_cast<std::int64_t>(evidence.frames);
    document.decodedSampleFramesKind = "exact";
    document.decodedSampleFramesTrust = "authoritative";
    document.decodedSampleFramesSource = presentationTotalSourceName(evidence.source);
    document.decodedSampleFramesBeforeGaplessCorrection =
        static_cast<std::int64_t>(table.physicalFrames);
    document.skipSamplesStart = static_cast<std::int64_t>(table.initialSkipFrames);
    document.skipSamplesEnd = static_cast<std::int64_t>(table.terminalDiscardFrames);
    document.skipSamplesTotal = document.skipSamplesStart + document.skipSamplesEnd;
    document.gaplessCorrectedDecodedSampleFrames =
        static_cast<std::int64_t>(evidence.frames);
    document.gaplessCorrectionApplied = document.skipSamplesTotal > 0;
    document.gaplessCorrectionSource =
        "mp4_mp3_sample_edit_table_presentation";
    document.frameCountPolicyReason =
        "validated selected MP3 sample and edit tables prove exact presentation";
    document.durationSec = static_cast<double>(evidence.frames) /
        static_cast<double>(evidence.sampleRate);
    document.durationKind = "exact";
    document.durationEstimationMethod = "from_mp4_mp3_sample_edit_tables";
    document.mp4Mp3SampleTableGenericScanSkipped = true;
    updateEstimatedDecodedBytes(document);
}

void copyOggOpusSequentialDiagnostics(
    FastProbeJsonDocument& document,
    const OggOpusSequentialPresentationResult& scan) {
    document.oggOpusSequentialStatus = oggOpusSequentialStatusName(scan.status);
    document.oggOpusSequentialReason = oggOpusSequentialReasonName(scan.reason);
    document.oggOpusSequentialSelectedSerial = scan.selectedSerial;
    document.oggOpusSequentialPreSkip = scan.preSkip;
    document.oggOpusSequentialPhysicalPacketFrames = scan.physicalPacketFrames;
    document.oggOpusSequentialLastPacketDuration = scan.lastPacketDuration;
    document.oggOpusSequentialEosGranule = scan.eosGranule;
    document.oggOpusSequentialTerminalDiscard = scan.terminalDiscard;
    document.oggOpusSequentialPresentationFrames = scan.presentationFrames;
    document.oggOpusSequentialFileSizeBytes = scan.fileSizeBytes;
    document.oggOpusSequentialFileIndex = scan.fileIndex;
    document.oggOpusSequentialLastWriteTime100ns = scan.lastWriteTime100ns;
    document.oggOpusSequentialVolumeSerialNumber = scan.volumeSerialNumber;
    document.oggOpusSequentialBytesReturned = scan.bytesReturned;
    document.oggOpusSequentialUniqueBytes = scan.uniqueBytes;
    document.oggOpusSequentialDuplicateBytes = scan.duplicateBytes;
    document.oggOpusSequentialReadCalls = scan.readCalls;
    document.oggOpusSequentialSeekCallsAfterOpen = scan.seekCallsAfterOpen;
    document.oggOpusSequentialScanDurationUs = scan.scanDurationUs;
    document.oggOpusSequentialPagesParsed = scan.pagesParsed;
    document.oggOpusSequentialSelectedPages = scan.selectedPages;
    document.oggOpusSequentialSelectedAudioPackets = scan.selectedAudioPackets;
    document.oggOpusSequentialMaximumPacketBytes = scan.maximumPacketBytes;
    document.oggOpusSequentialMaximumWorkingBufferBytes = scan.maximumWorkingBufferBytes;
    document.oggOpusSequentialReachedEof = scan.reachedPhysicalEof;
    document.oggOpusSequentialAllPageCrcValid = scan.allPageCrcValid;
    document.oggOpusSequentialSelectedSequenceContinuous =
        scan.selectedSequenceContinuous;
    document.oggOpusSequentialPacketContinuityValid = scan.packetContinuityValid;
    document.oggOpusSequentialFinalGranuleInPacketInterval =
        scan.finalGranuleInPacketInterval;
}

void applyOggOpusSequentialPresentationAuthority(
    FastProbeResult& result,
    const std::string& path,
    const AVFormatContext* formatContext,
    const AVStream* audioStream) {
    const bool strongerExactAuthority =
        result.totalPresentation.trust == PresentationTotalTrust::SampleExact;
    const OggOpusSequentialEligibility eligibility =
        evaluateOggOpusSequentialEligibility(
            path, formatContext, audioStream, strongerExactAuthority);
    FastProbeJsonDocument& document = result.document;
    document.oggOpusSequentialEligible = eligibility.eligible;
    if (!eligibility.eligible) {
        document.oggOpusSequentialReason =
            oggOpusSequentialReasonName(eligibility.reason);
        return;
    }

    document.oggOpusSequentialEntered = true;
    result.oggOpusSequentialPresentation =
        probeOggOpusSequentialPresentation(path, eligibility.selected);
    copyOggOpusSequentialDiagnostics(
        document, result.oggOpusSequentialPresentation);
    if (!result.oggOpusSequentialPresentation.exact()) {
        return;
    }

    TotalPresentationEvidence evidence =
        makeOggOpusSequentialTotalPresentationEvidence(
            result.oggOpusSequentialPresentation);
    evidence = reconcileTotalPresentationEvidence(evidence, result.totalPresentation);
    if (evidence.conflict) {
        result.oggOpusSequentialPresentation.status =
            OggOpusSequentialStatus::Conflict;
        result.oggOpusSequentialPresentation.reason =
            OggOpusSequentialReason::IndependentExactAuthorityConflict;
        copyOggOpusSequentialDiagnostics(
            document, result.oggOpusSequentialPresentation);
        return;
    }
    if (evidence.frames == 0 || evidence.frames >
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        return;
    }

    result.totalPresentation = evidence;
    document.decodedSampleFrames = static_cast<std::int64_t>(evidence.frames);
    document.decodedSampleFramesKind = "exact";
    document.decodedSampleFramesTrust = "authoritative";
    document.decodedSampleFramesSource = presentationTotalSourceName(evidence.source);
    document.frameCountPolicyReason =
        "validated sequential Ogg Opus continuity proves the presentation end";
    document.durationSec = static_cast<double>(evidence.frames) /
        static_cast<double>(evidence.sampleRate);
    document.durationKind = "exact";
    document.durationEstimationMethod = "from_ogg_opus_sequential_presentation";
    document.oggOpusSequentialGenericScanSkipped = true;
    updateEstimatedDecodedBytes(document);
}

FrameCountPolicyState makeFrameCountPolicyState(const FastProbeJsonDocument& document) {
    FrameCountPolicyState state;
    state.formatName = document.formatName;
    state.selectedAudio.codecId = static_cast<AVCodecID>(document.selectedAudio.codecId);
    state.selectedAudio.codecName = document.selectedAudio.codecName;
    state.selectedAudio.sampleRate = document.selectedAudio.sampleRate;
    state.selectedAudio.channels = document.selectedAudio.channels;
    state.decodedSampleFrames = document.decodedSampleFrames;
    state.decodedSampleFramesKind = document.decodedSampleFramesKind;
    state.decodedSampleFramesTrust = document.decodedSampleFramesTrust;
    state.decodedSampleFramesSource = document.decodedSampleFramesSource;
    state.decodedSampleFramesBeforeCorrection = document.decodedSampleFramesBeforeCorrection;
    state.packetPtsSpanFrames = document.packetPtsSpanFrames;
    state.packetDurationSumFrames = document.packetDurationSumFrames;
    state.packetFrameCountCandidateUsed = document.packetFrameCountCandidateUsed;
    state.frameCountPolicyReason = document.frameCountPolicyReason;
    state.decodedSampleFramesBeforeGaplessCorrection =
        document.decodedSampleFramesBeforeGaplessCorrection;
    state.skipSamplesStart = document.skipSamplesStart;
    state.skipSamplesEnd = document.skipSamplesEnd;
    state.skipSamplesTotal = document.skipSamplesTotal;
    state.gaplessCorrectedDecodedSampleFrames = document.gaplessCorrectedDecodedSampleFrames;
    state.gaplessCorrectionApplied = document.gaplessCorrectionApplied;
    state.gaplessCorrectionSource = document.gaplessCorrectionSource;
    state.gaplessSideDataPacketCount = document.gaplessSideDataPacketCount;
    state.gaplessAudioPacketsScanned = document.gaplessAudioPacketsScanned;
    state.estimatedDecodedBytes = document.estimatedDecodedBytes;
    state.estimatedDecodedBytesKind = document.estimatedDecodedBytesKind;
    state.warnings = document.warnings;
    return state;
}

void applyFrameCountPolicyState(FastProbeJsonDocument& document, const FrameCountPolicyState& state) {
    document.decodedSampleFrames = state.decodedSampleFrames;
    document.decodedSampleFramesKind = state.decodedSampleFramesKind;
    document.decodedSampleFramesTrust = state.decodedSampleFramesTrust;
    document.decodedSampleFramesSource = state.decodedSampleFramesSource;
    document.decodedSampleFramesBeforeCorrection = state.decodedSampleFramesBeforeCorrection;
    document.packetPtsSpanFrames = state.packetPtsSpanFrames;
    document.packetDurationSumFrames = state.packetDurationSumFrames;
    document.packetFrameCountCandidateUsed = state.packetFrameCountCandidateUsed;
    document.frameCountPolicyReason = state.frameCountPolicyReason;
    document.decodedSampleFramesBeforeGaplessCorrection =
        state.decodedSampleFramesBeforeGaplessCorrection;
    document.skipSamplesStart = state.skipSamplesStart;
    document.skipSamplesEnd = state.skipSamplesEnd;
    document.skipSamplesTotal = state.skipSamplesTotal;
    document.gaplessCorrectedDecodedSampleFrames = state.gaplessCorrectedDecodedSampleFrames;
    document.gaplessCorrectionApplied = state.gaplessCorrectionApplied;
    document.gaplessCorrectionSource = state.gaplessCorrectionSource;
    document.gaplessSideDataPacketCount = state.gaplessSideDataPacketCount;
    document.gaplessAudioPacketsScanned = state.gaplessAudioPacketsScanned;
    document.estimatedDecodedBytes = state.estimatedDecodedBytes;
    document.estimatedDecodedBytesKind = state.estimatedDecodedBytesKind;
    document.warnings = state.warnings;
}

void copyMatroskaAacSequentialDiagnostics(
    FastProbeJsonDocument& document,
    const MatroskaAacSequentialPresentationResult& scan) {
    document.matroskaAacSequentialStatus =
        matroskaAacSequentialStatusName(scan.status);
    document.matroskaAacSequentialReason =
        matroskaAacSequentialReasonName(scan.reason);
    document.matroskaAacSequentialTrackNumber = scan.trackNumber;
    document.matroskaAacSequentialTrackUid = scan.trackUid;
    document.matroskaAacSequentialAacObjectType = scan.aacObjectType;
    document.matroskaAacSequentialSampleRate = scan.sampleRate;
    document.matroskaAacSequentialChannels = scan.channels;
    document.matroskaAacSequentialSamplesPerAccessUnit =
        scan.samplesPerAccessUnit;
    document.matroskaAacSequentialSelectedAccessUnits =
        scan.selectedAccessUnits;
    document.matroskaAacSequentialPhysicalFrames = scan.physicalFrames;
    document.matroskaAacSequentialCodecDelayNs =
        scan.codecDelayNanoseconds;
    document.matroskaAacSequentialInitialSkipFrames =
        scan.initialSkipFrames;
    document.matroskaAacSequentialDiscardPaddingNs =
        scan.finalDiscardPaddingNanoseconds;
    document.matroskaAacSequentialTerminalDiscardFrames =
        scan.terminalDiscardFrames;
    document.matroskaAacSequentialPresentationFrames =
        scan.presentationFrames;
    document.matroskaAacSequentialFileSizeBytes = scan.fileSizeBytes;
    document.matroskaAacSequentialBytesReturned = scan.bytesReturned;
    document.matroskaAacSequentialReadCalls = scan.readCalls;
    document.matroskaAacSequentialScanDurationUs = scan.scanDurationUs;
    document.matroskaAacSequentialMaximumWorkingBufferBytes =
        scan.maximumWorkingBufferBytes;
    document.matroskaAacSequentialElementsParsed = scan.elementsParsed;
    document.matroskaAacSequentialClustersParsed = scan.clustersParsed;
    document.matroskaAacSequentialSelectedBlocks = scan.selectedBlocks;
    document.matroskaAacSequentialSelectedLaces = scan.selectedLaces;
    document.matroskaAacSequentialReachedEof = scan.reachedPhysicalEof;
    document.matroskaAacSequentialReachedSegmentEnd =
        scan.reachedSegmentEnd;
    document.matroskaAacSequentialTrackMappingValid =
        scan.selectedTrackMappingValid;
    document.matroskaAacSequentialTimestampContinuityValid =
        scan.timestampContinuityValid;
    document.matroskaAacSequentialAllRelevantCrcValid =
        scan.allRelevantCrcValid;
    document.matroskaAacSequentialCheckedArithmeticValid =
        scan.checkedArithmeticValid;
    document.matroskaAacSequentialLateFallback =
        scan.status == MatroskaAacSequentialStatus::UnsupportedLate;
}

void applyMatroskaAacSequentialPresentationAuthority(
    FastProbeResult& result,
    const std::string& path,
    const AVFormatContext* formatContext,
    const AVStream* audioStream) {
    const AVCodecParameters* codecpar =
        audioStream ? audioStream->codecpar : nullptr;
    if (!codecpar) {
        return;
    }

    const FrameCountPolicyState policyState =
        makeFrameCountPolicyState(result.document);
    const bool wouldRequireGenericPacketScan =
        result.document.decodedSampleFramesKind != "exact" &&
        (shouldScanPacketFrameCountCandidates(policyState) ||
         codecpar->initial_padding > 0 ||
         codecpar->trailing_padding > 0);
    if (!wouldRequireGenericPacketScan) {
        return;
    }

    const bool strongerExactAuthority =
        result.totalPresentation.trust == PresentationTotalTrust::SampleExact;
    const MatroskaAacSequentialEligibility eligibility =
        evaluateMatroskaAacSequentialEligibility(
            path, formatContext, audioStream, strongerExactAuthority);
    FastProbeJsonDocument& document = result.document;
    document.matroskaAacSequentialEligible = eligibility.eligible;
    if (!eligibility.eligible) {
        document.matroskaAacSequentialReason =
            matroskaAacSequentialReasonName(eligibility.reason);
        return;
    }

    document.matroskaAacSequentialEntered = true;
    result.matroskaAacSequentialPresentation =
        probeMatroskaAacSequentialPresentation(path, eligibility.selected);
    copyMatroskaAacSequentialDiagnostics(
        document, result.matroskaAacSequentialPresentation);
    if (!result.matroskaAacSequentialPresentation.exact()) {
        return;
    }

    TotalPresentationEvidence evidence =
        makeMatroskaAacSequentialTotalPresentationEvidence(
            result.matroskaAacSequentialPresentation);
    evidence = reconcileTotalPresentationEvidence(
        evidence, result.totalPresentation);
    if (evidence.conflict) {
        result.matroskaAacSequentialPresentation.status =
            MatroskaAacSequentialStatus::Conflict;
        result.matroskaAacSequentialPresentation.reason =
            MatroskaAacSequentialReason::IndependentExactAuthorityConflict;
        copyMatroskaAacSequentialDiagnostics(
            document, result.matroskaAacSequentialPresentation);
        return;
    }
    if (evidence.frames == 0 || evidence.frames >
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        return;
    }

    result.totalPresentation = evidence;
    document.decodedSampleFrames = static_cast<std::int64_t>(evidence.frames);
    document.decodedSampleFramesKind = "exact";
    document.decodedSampleFramesTrust = "authoritative";
    document.decodedSampleFramesSource =
        presentationTotalSourceName(evidence.source);
    document.frameCountPolicyReason =
        "validated sequential Matroska AAC selected-track continuity "
        "proves the presentation end";
    document.durationSec = static_cast<double>(evidence.frames) /
        static_cast<double>(evidence.sampleRate);
    document.durationKind = "exact";
    document.durationEstimationMethod =
        "from_matroska_aac_sequential_presentation";
    document.matroskaAacSequentialGenericScanSkipped = true;
    updateEstimatedDecodedBytes(document);
}

void copyAdtsAacSequentialDiagnostics(
    FastProbeJsonDocument& document,
    const AdtsAacSequentialPresentationResult& scan) {
    document.adtsAacSequentialStatus = adtsAacSequentialStatusName(scan.status);
    document.adtsAacSequentialReason = adtsAacSequentialReasonName(scan.reason);
    document.adtsAacSequentialMpegId = scan.mpegId;
    document.adtsAacSequentialAudioObjectType = scan.audioObjectType;
    document.adtsAacSequentialSampleRate = scan.sampleRate;
    document.adtsAacSequentialChannels = scan.channels;
    document.adtsAacSequentialChannelConfiguration = scan.channelConfiguration;
    document.adtsAacSequentialProtectionAbsent = scan.protectionAbsent;
    document.adtsAacSequentialFrameCount = scan.frameCount;
    document.adtsAacSequentialRawDataBlockCount = scan.rawDataBlockCount;
    document.adtsAacSequentialSamplesPerRawDataBlock = scan.samplesPerRawDataBlock;
    document.adtsAacSequentialPhysicalFrames = scan.physicalFrames;
    document.adtsAacSequentialPresentationFrames = scan.presentationFrames;
    document.adtsAacSequentialFileSizeBytes = scan.fileSizeBytes;
    document.adtsAacSequentialFileIndex = scan.fileIndex;
    document.adtsAacSequentialLastWriteTime100ns = scan.lastWriteTime100ns;
    document.adtsAacSequentialVolumeSerialNumber = scan.volumeSerialNumber;
    document.adtsAacSequentialBytesReturned = scan.bytesReturned;
    document.adtsAacSequentialUniqueBytes = scan.uniqueBytes;
    document.adtsAacSequentialDuplicateBytes = scan.duplicateBytes;
    document.adtsAacSequentialReadCalls = scan.readCalls;
    document.adtsAacSequentialSeekCallsAfterOpen = scan.seekCallsAfterOpen;
    document.adtsAacSequentialMaximumFrameBytes = scan.maximumFrameBytes;
    document.adtsAacSequentialScanDurationUs = scan.scanDurationUs;
    document.adtsAacSequentialMaximumWorkingBufferBytes =
        scan.maximumWorkingBufferBytes;
    document.adtsAacSequentialReachedPhysicalEof = scan.reachedPhysicalEof;
    document.adtsAacSequentialFrameBoundariesValid = scan.frameBoundariesValid;
    document.adtsAacSequentialConfigurationContinuous = scan.configurationContinuous;
    document.adtsAacSequentialOutputDomainValidated = scan.outputDomainValidated;
    document.adtsAacSequentialCheckedArithmeticValid = scan.checkedArithmeticValid;
    document.adtsAacSequentialFileIdentityStable = scan.fileIdentityStable;
    document.adtsAacSequentialLateFallback =
        scan.status == AdtsAacSequentialStatus::UnsupportedLate ||
        ((scan.status == AdtsAacSequentialStatus::Conflict ||
         scan.status == AdtsAacSequentialStatus::InvalidMedia ||
          scan.status == AdtsAacSequentialStatus::IoError) &&
         scan.frameCount > 0);
}

void applyAdtsAacSequentialPresentationAuthority(
    FastProbeResult& result,
    const std::string& path,
    const AVFormatContext* formatContext,
    const AVStream* audioStream) {
    const AVCodecParameters* codecpar =
        audioStream ? audioStream->codecpar : nullptr;
    if (!codecpar) {
        return;
    }
    const FrameCountPolicyState policyState =
        makeFrameCountPolicyState(result.document);
    const bool wouldRequireGenericPacketScan =
        result.document.decodedSampleFramesKind != "exact" &&
        (shouldScanPacketFrameCountCandidates(policyState) ||
         codecpar->initial_padding > 0 || codecpar->trailing_padding > 0);
    if (!wouldRequireGenericPacketScan) {
        return;
    }

    const bool strongerExactAuthority =
        result.totalPresentation.trust == PresentationTotalTrust::SampleExact;
    const AdtsAacSequentialEligibility eligibility =
        evaluateAdtsAacSequentialEligibility(
            path, formatContext, audioStream, strongerExactAuthority);
    FastProbeJsonDocument& document = result.document;
    document.adtsAacSequentialEligible = eligibility.eligible;
    if (!eligibility.eligible) {
        document.adtsAacSequentialReason =
            adtsAacSequentialReasonName(eligibility.reason);
        return;
    }

    document.adtsAacSequentialEntered = true;
    result.adtsAacSequentialPresentation =
        probeAdtsAacSequentialPresentation(path, eligibility.selected);
    copyAdtsAacSequentialDiagnostics(
        document, result.adtsAacSequentialPresentation);
    if (!result.adtsAacSequentialPresentation.exact()) {
        return;
    }

    TotalPresentationEvidence evidence =
        makeAdtsAacSequentialTotalPresentationEvidence(
            result.adtsAacSequentialPresentation);
    evidence = reconcileTotalPresentationEvidence(evidence, result.totalPresentation);
    if (evidence.conflict) {
        result.adtsAacSequentialPresentation.status =
            AdtsAacSequentialStatus::Conflict;
        result.adtsAacSequentialPresentation.reason =
            AdtsAacSequentialReason::ExactAuthorityConflict;
        copyAdtsAacSequentialDiagnostics(
            document, result.adtsAacSequentialPresentation);
        return;
    }
    if (evidence.frames == 0 || evidence.frames >
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        return;
    }

    result.totalPresentation = evidence;
    document.decodedSampleFrames = static_cast<std::int64_t>(evidence.frames);
    document.decodedSampleFramesKind = "exact";
    document.decodedSampleFramesTrust = "authoritative";
    document.decodedSampleFramesSource = presentationTotalSourceName(evidence.source);
    document.frameCountPolicyReason =
        "validated sequential ADTS AAC frame inventory proves the presentation end";
    document.durationSec = static_cast<double>(evidence.frames) /
        static_cast<double>(evidence.sampleRate);
    document.durationKind = "exact";
    document.durationEstimationMethod =
        "from_adts_aac_sequential_presentation";
    document.adtsAacSequentialGenericScanSkipped = true;
    updateEstimatedDecodedBytes(document);
}

void copyDolbySequentialDiagnostics(
    FastProbeJsonDocument& document,
    const DolbySequentialPresentationResult& scan) {
    document.dolbySequentialStatus = dolbySequentialStatusName(scan.status);
    document.dolbySequentialReason = dolbySequentialReasonName(scan.reason);
    document.dolbySequentialCodecFamily =
        dolbySequentialCodecFamilyName(scan.family);
    document.dolbySequentialSampleRate = scan.sampleRate;
    document.dolbySequentialChannels = scan.channels;
    document.dolbySequentialBitstreamId = scan.bitstreamId;
    document.dolbySequentialChannelMode = scan.channelMode;
    document.dolbySequentialLfe = scan.lfe;
    document.dolbySequentialSelectedStreamType = scan.selectedStreamType;
    document.dolbySequentialSelectedSubstreamId = scan.selectedSubstreamId;
    document.dolbySequentialSyncframeCount = scan.syncframeCount;
    document.dolbySequentialAc3FrameCount = scan.ac3FrameCount;
    document.dolbySequentialEac3IndependentFrameCount =
        scan.eac3IndependentFrameCount;
    document.dolbySequentialEac3DependentFrameCount =
        scan.eac3DependentFrameCount;
    document.dolbySequentialAudioBlockCount = scan.audioBlockCount;
    document.dolbySequentialSamplesPerAudioBlock = scan.samplesPerAudioBlock;
    document.dolbySequentialPresentationFrames = scan.presentationFrames;
    document.dolbySequentialFileSizeBytes = scan.fileSizeBytes;
    document.dolbySequentialFileIndex = scan.fileIndex;
    document.dolbySequentialLastWriteTime100ns = scan.lastWriteTime100ns;
    document.dolbySequentialVolumeSerialNumber = scan.volumeSerialNumber;
    document.dolbySequentialBytesReturned = scan.bytesReturned;
    document.dolbySequentialUniqueBytes = scan.uniqueBytes;
    document.dolbySequentialDuplicateBytes = scan.duplicateBytes;
    document.dolbySequentialReadCalls = scan.readCalls;
    document.dolbySequentialSeekCallsAfterOpen = scan.seekCallsAfterOpen;
    document.dolbySequentialMaximumFrameBytes = scan.maximumFrameBytes;
    document.dolbySequentialScanDurationUs = scan.scanDurationUs;
    document.dolbySequentialMaximumWorkingBufferBytes =
        scan.maximumWorkingBufferBytes;
    document.dolbySequentialReachedPhysicalEof = scan.reachedPhysicalEof;
    document.dolbySequentialFrameBoundariesValid = scan.frameBoundariesValid;
    document.dolbySequentialConfigurationContinuous = scan.configurationContinuous;
    document.dolbySequentialSubstreamPolicyValid = scan.substreamPolicyValid;
    document.dolbySequentialOutputDomainValidated = scan.outputDomainValidated;
    document.dolbySequentialCheckedArithmeticValid = scan.checkedArithmeticValid;
    document.dolbySequentialFileIdentityStable = scan.fileIdentityStable;
    document.dolbySequentialCrcObserved = scan.crcObserved;
    document.dolbySequentialCrcValidated = scan.crcValidated;
    document.dolbySequentialPayloadValiditySeparatedFromExtent =
        scan.payloadValiditySeparatedFromExtent;
    document.dolbySequentialLateFallback =
        scan.status == DolbySequentialPresentationStatus::UnsupportedLate ||
        ((scan.status == DolbySequentialPresentationStatus::Conflict ||
          scan.status == DolbySequentialPresentationStatus::InvalidMedia ||
          scan.status == DolbySequentialPresentationStatus::IoError) &&
         scan.syncframeCount > 0);
}

void applyDolbySequentialPresentationAuthority(
    FastProbeResult& result,
    const std::string& path,
    const AVFormatContext* formatContext,
    const AVStream* audioStream) {
    const bool strongerExactAuthority =
        result.totalPresentation.trust == PresentationTotalTrust::SampleExact;
    const DolbySequentialEligibility eligibility =
        evaluateDolbySequentialEligibility(
            path, formatContext, audioStream, strongerExactAuthority);
    FastProbeJsonDocument& document = result.document;
    document.dolbySequentialEligible = eligibility.eligible;
    if (!eligibility.eligible) {
        document.dolbySequentialReason =
            dolbySequentialReasonName(eligibility.reason);
        return;
    }

    document.dolbySequentialEntered = true;
    result.dolbySequentialPresentation =
        probeDolbySequentialPresentation(path, eligibility.selected);
    copyDolbySequentialDiagnostics(document, result.dolbySequentialPresentation);
    if (!result.dolbySequentialPresentation.exact()) {
        return;
    }

    TotalPresentationEvidence evidence =
        makeDolbySequentialTotalPresentationEvidence(
            result.dolbySequentialPresentation);
    evidence = reconcileTotalPresentationEvidence(evidence, result.totalPresentation);
    if (evidence.conflict) {
        result.dolbySequentialPresentation.status =
            DolbySequentialPresentationStatus::Conflict;
        result.dolbySequentialPresentation.reason =
            DolbySequentialPresentationReason::ExactAuthorityConflict;
        copyDolbySequentialDiagnostics(
            document, result.dolbySequentialPresentation);
        return;
    }
    if (evidence.frames == 0 || evidence.frames >
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        return;
    }

    result.totalPresentation = evidence;
    document.decodedSampleFrames = static_cast<std::int64_t>(evidence.frames);
    document.decodedSampleFramesKind = "exact";
    document.decodedSampleFramesTrust = "authoritative";
    document.decodedSampleFramesSource = presentationTotalSourceName(evidence.source);
    document.frameCountPolicyReason =
        "validated sequential raw Dolby syncframe inventory proves the presentation end";
    document.durationSec = static_cast<double>(evidence.frames) /
        static_cast<double>(evidence.sampleRate);
    document.durationKind = "exact";
    document.durationEstimationMethod =
        result.dolbySequentialPresentation.family == DolbySequentialCodecFamily::Ac3
            ? "from_ac3_sequential_presentation"
            : "from_eac3_sequential_presentation";
    document.dolbySequentialTypedEvidencePublished = true;
    updateEstimatedDecodedBytes(document);
}

void applyFastFrameCountPolicies(
    FastProbeResult& result,
    const std::string& path,
    const AVStream* audioStream,
    bool allowExactPacketPresentationScan) {
    const AVCodecParameters* codecpar = audioStream && audioStream->codecpar ? audioStream->codecpar : nullptr;
    if (!codecpar) {
        return;
    }

    FrameCountPolicyState policyState = makeFrameCountPolicyState(result.document);
    const PacketScanOptions packetScanOptions{
        kFastProbeSizeBytes,
        kFastProbeAnalyzeDurationUs
    };

    const bool legacyGaplessScanRequired =
        (codecpar->codec_id == AV_CODEC_ID_MP3 || codecpar->codec_id == AV_CODEC_ID_OPUS) &&
        result.document.decodedSampleFramesKind != "exact";
    const bool standaloneMp3Fallback =
        legacyGaplessScanRequired &&
        result.document.formatName == "mp3" &&
        codecpar->codec_id == AV_CODEC_ID_MP3;
    const bool preliminaryPaddingRequiresExactPacketScan =
        allowExactPacketPresentationScan &&
        result.document.decodedSampleFramesKind != "exact" &&
        (codecpar->initial_padding > 0 || codecpar->trailing_padding > 0);
    const bool packetScanRequired =
        shouldScanPacketFrameCountCandidates(policyState) ||
        preliminaryPaddingRequiresExactPacketScan ||
        standaloneMp3Fallback;
    const bool gaplessScanRequired =
        legacyGaplessScanRequired ||
        preliminaryPaddingRequiresExactPacketScan;
    const bool genericOggOpusScan =
        codecpar->codec_id == AV_CODEC_ID_OPUS &&
        result.document.formatName == "ogg" &&
        (packetScanRequired || gaplessScanRequired);
    if (genericOggOpusScan) {
        result.document.oggOpusSequentialGenericScanEntered = true;
        result.document.oggOpusSequentialGenericScanSkipped = false;
        result.document.oggOpusSequentialPossibleDoublePass =
            result.document.oggOpusSequentialEntered;
    }
    const bool genericMatroskaAacScan =
        codecpar->codec_id == AV_CODEC_ID_AAC &&
        result.document.formatName.find("matroska") != std::string::npos &&
        (packetScanRequired || gaplessScanRequired);
    if (genericMatroskaAacScan) {
        result.document.matroskaAacSequentialGenericScanEntered = true;
        result.document.matroskaAacSequentialGenericScanSkipped = false;
        result.document.matroskaAacSequentialPossibleDoublePass =
            result.document.matroskaAacSequentialEntered;
    }
    const bool genericAdtsAacScan =
        codecpar->codec_id == AV_CODEC_ID_AAC &&
        result.document.formatName == "aac" &&
        (packetScanRequired || gaplessScanRequired);
    if (genericAdtsAacScan) {
        result.document.adtsAacSequentialGenericScanEntered = true;
        result.document.adtsAacSequentialGenericScanSkipped = false;
        result.document.adtsAacSequentialPossibleDoublePass =
            result.document.adtsAacSequentialEntered &&
            result.document.adtsAacSequentialLateFallback;
    }
    const bool genericDolbyScan =
        (codecpar->codec_id == AV_CODEC_ID_AC3 ||
         codecpar->codec_id == AV_CODEC_ID_EAC3) &&
        (result.document.formatName == "ac3" ||
         result.document.formatName == "eac3") &&
        (packetScanRequired || gaplessScanRequired);
    if (genericDolbyScan) {
        result.document.dolbySequentialGenericFullScanEntered = true;
        result.document.dolbySequentialGenericFullScanSkipped = false;
        result.document.dolbySequentialPossibleDoublePass =
            result.document.dolbySequentialEntered &&
            result.document.dolbySequentialLateFallback;
    }
    const bool genericMp4Mp3Scan =
        codecpar->codec_id == AV_CODEC_ID_MP3 &&
        result.document.formatName.find("mov") != std::string::npos &&
        (packetScanRequired || gaplessScanRequired);
    if (genericMp4Mp3Scan) {
        result.document.mp4Mp3SampleTableGenericScanEntered = true;
        result.document.mp4Mp3SampleTableGenericScanSkipped = false;
        result.document.mp4Mp3SampleTablePossibleDoublePass =
            result.document.mp4Mp3SampleTableEntered;
    }

    PacketFrameCountScan packetScan;
    GaplessSkipSampleScan gaplessScan;
    if (packetScanRequired && gaplessScanRequired) {
        const AudioPresentationEvidenceScan evidence =
            scanAudioPresentationEvidence(
                path,
                static_cast<int>(audioStream->index),
                policyState.selectedAudio.sampleRate,
                codecpar->codec_id,
                packetScanOptions);
        packetScan = evidence.packetTiming;
        gaplessScan = evidence.gapless;
    } else if (packetScanRequired) {
        packetScan = scanPacketFrameCountCandidates(
            path,
            static_cast<int>(audioStream->index),
            policyState.selectedAudio.sampleRate,
            codecpar->codec_id,
            packetScanOptions);
    } else if (gaplessScanRequired) {
        gaplessScan = scanGaplessSkipSampleSideData(
            path,
            static_cast<int>(audioStream->index),
            packetScanOptions);
    }

    // Resolve complete packet evidence after traversal, including padding
    // authority that was visible only in packet side data.
    const bool evaluateExactPacketPresentation =
        shouldEvaluateExactPacketPresentationAfterScan(
            policyState,
            allowExactPacketPresentationScan,
            packetScanRequired,
            gaplessScanRequired);
    bool exactPacketPresentationApplied = false;
    if (evaluateExactPacketPresentation) {
        exactPacketPresentationApplied = applyExactPacketPresentationBudget(
            policyState,
            AudioPresentationEvidenceScan{packetScan, gaplessScan},
            exactFramesFromSampleDomainStreamDuration(audioStream),
            standaloneMp3Fallback);
    }
    if (!exactPacketPresentationApplied) {
        if (legacyGaplessScanRequired) {
            if (codecpar->codec_id == AV_CODEC_ID_MP3) {
                applyGaplessSkipSampleCorrection(policyState, gaplessScan);
            } else {
                recordGaplessSkipSampleScan(policyState, gaplessScan);
            }
        } else if (preliminaryPaddingRequiresExactPacketScan) {
            recordGaplessSkipSampleScan(policyState, gaplessScan);
        }
    }
    if (packetScanRequired) {
        applyPacketFrameCountPolicies(policyState, packetScan);
    }

    applyFrameCountPolicyState(result.document, policyState);
    if (policyState.decodedSampleFramesKind == "exact" &&
        policyState.decodedSampleFramesSource == "exact_packet_presentation" &&
        policyState.decodedSampleFrames > 0) {
        result.totalPresentation.frames =
            static_cast<std::uint64_t>(policyState.decodedSampleFrames);
        result.totalPresentation.trust = PresentationTotalTrust::SampleExact;
        result.totalPresentation.source = PresentationTotalSource::ExactPacketPresentation;
        result.totalPresentation.domain = PresentationSampleDomain::NativeStreamSamples;
        result.totalPresentation.sampleRate = policyState.selectedAudio.sampleRate;
        result.totalPresentation.exactRescale = true;
        result.totalPresentation.conflict = false;
        result.totalPresentation.validation =
            PresentationTotalValidation::SelfContainedMetadata;
    }
}

void finalizeFrameCountTrustPolicy(FastProbeResult& result, const AVStream* audioStream) {
    const AVCodecParameters* codecpar = audioStream && audioStream->codecpar ? audioStream->codecpar : nullptr;
    FrameCountPolicyState policyState = makeFrameCountPolicyState(result.document);
    finalizeFrameCountTrustPolicy(
        policyState,
        codecpar != nullptr,
        codecpar && isPcmCodec(codecpar->codec_id));
    applyFrameCountPolicyState(result.document, policyState);
}

}  // namespace

std::string rationalToString(AVRational value) {
    std::ostringstream out;
    out << value.num << "/" << value.den;
    return out.str();
}

StreamSummary makeStreamSummary(int index, const AVStream* stream) {
    StreamSummary summary;
    summary.index = index;
    if (!stream || !stream->codecpar) {
        return summary;
    }

    const AVCodecParameters* codecpar = stream->codecpar;
    summary.mediaType = mediaTypeName(codecpar->codec_type);
    summary.codecName = avcodec_get_name(codecpar->codec_id);
    summary.codecId = static_cast<int>(codecpar->codec_id);
    summary.sampleRate = codecpar->sample_rate;
    summary.channels = codecpar->ch_layout.nb_channels;
    summary.bitRate = codecpar->bit_rate;
    summary.timeBase = rationalToString(stream->time_base);
    return summary;
}

FastProbeResult runFastProbe(const std::string& path) {
    FastProbeResult result;
    result.document.sourcePath = path;

    Ffmpeg::UniqueAVFormatContext formatContext(avformat_alloc_context());
    if (!formatContext) {
        result.document.errors.push_back("avformat_alloc_context failed");
        return result;
    }

    formatContext->probesize = kFastProbeSizeBytes;
    formatContext->max_analyze_duration = kFastProbeAnalyzeDurationUs;

    AVDictionary* options = nullptr;
    av_dict_set(&options, "probesize", "4194304", 0);
    av_dict_set(&options, "analyzeduration", "3000000", 0);

    AVFormatContext* openContext = formatContext.release();
    int ret = avformat_open_input(&openContext, path.c_str(), nullptr, &options);
    formatContext.reset(openContext);
    av_dict_free(&options);
    if (ret < 0) {
        result.document.errors.push_back("avformat_open_input failed: " + ffErrorString(ret));
        return result;
    }

    ret = avformat_find_stream_info(formatContext.get(), nullptr);
    fillFastSourceInfo(result, formatContext.get());
    if (ret < 0) {
        result.document.errors.push_back("avformat_find_stream_info failed: " + ffErrorString(ret));
        return result;
    }

    result.streamInfoFound = true;
    fillFastStreamDetails(result, formatContext.get());

    const Ffmpeg::AudioStreamSelection selection =
        Ffmpeg::selectBestAudioStreamWithFirstAudioFallback(formatContext.get());
    if (selection.streamIndex < 0) {
        result.document.errors.push_back("no audio stream found: " + ffErrorString(selection.bestStreamResult));
        return result;
    }
    if (selection.usedFirstAudioFallback) {
        result.document.warnings.push_back(
            "av_find_best_stream(audio) failed, using first audio stream: " +
            ffErrorString(selection.bestStreamResult));
    }

    const int audioStreamIndex = selection.streamIndex;
    const AVCodec* decoder = selection.decoder;
    result.document.bestAudioStreamIndex = audioStreamIndex;
    AVStream* audioStream = formatContext->streams[audioStreamIndex];
    if (!decoder && audioStream && audioStream->codecpar) {
        decoder = avcodec_find_decoder(audioStream->codecpar->codec_id);
    }
    fillFastSelectedAudio(result, audioStream, decoder);
    estimateFastDurationAndFrames(result, formatContext.get(), audioStream);
    applyMp3HeaderPresentationAuthority(result, path, formatContext.get(), audioStream);
    applyMp4Mp3SampleEditTablePresentationAuthority(
        result, path, formatContext.get(), audioStream);
    applyNutBoundedTailAuthority(result, path, formatContext.get(), audioStream);
    applyOggOpusSequentialPresentationAuthority(
        result, path, formatContext.get(), audioStream);
    applyMatroskaAacSequentialPresentationAuthority(
        result, path, formatContext.get(), audioStream);
    applyAdtsAacSequentialPresentationAuthority(
        result, path, formatContext.get(), audioStream);
    applyDolbySequentialPresentationAuthority(
        result, path, formatContext.get(), audioStream);
    applyFastFrameCountPolicies(result, path, audioStream, true);
    finalizeFrameCountTrustPolicy(result, audioStream);
    return result;
}

bool writeFastProbeJson(
    const std::filesystem::path& outputPath,
    const FastProbeResult& result,
    std::string& error) {
    return writeProbeJson(outputPath, result.document, error);
}

bool estimateDecodedBytesForPreflight(
    const AVFormatContext* formatContext,
    const AVStream* audioStream,
    const std::string& path,
    std::int64_t& estimatedFrames,
    std::int64_t& estimatedBytes,
    std::string& estimateKind) {
    FastProbeResult estimate;
    fillFastSourceInfo(estimate, formatContext);
    fillFastSelectedAudio(estimate, audioStream, nullptr);
    estimateFastDurationAndFrames(estimate, formatContext, audioStream);
    // Loading authority is resolved by runFastProbe; disk preflight must not
    // repeat the full packet traversal only to refine its byte estimate.
    applyFastFrameCountPolicies(estimate, path, audioStream, false);
    finalizeFrameCountTrustPolicy(estimate, audioStream);
    estimatedFrames = estimate.document.decodedSampleFrames;
    estimatedBytes = estimate.document.estimatedDecodedBytes;
    estimateKind = estimate.document.estimatedDecodedBytesKind;
    return estimatedBytes > 0 && estimateKind != "unknown";
}

}  // namespace AveMediaBridge::Probe
