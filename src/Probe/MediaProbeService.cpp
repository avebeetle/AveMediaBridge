#include "MediaProbeService.hpp"

#include "FrameCountPolicy.hpp"
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
    const bool preliminaryPaddingRequiresExactPacketScan =
        allowExactPacketPresentationScan &&
        result.document.decodedSampleFramesKind != "exact" &&
        (codecpar->initial_padding > 0 || codecpar->trailing_padding > 0);
    const bool packetScanRequired =
        shouldScanPacketFrameCountCandidates(policyState) ||
        preliminaryPaddingRequiresExactPacketScan;
    const bool gaplessScanRequired =
        legacyGaplessScanRequired ||
        preliminaryPaddingRequiresExactPacketScan;

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
            exactFramesFromSampleDomainStreamDuration(audioStream));
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
