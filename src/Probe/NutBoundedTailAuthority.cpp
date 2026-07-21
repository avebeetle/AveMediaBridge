#include "NutBoundedTailAuthority.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace AveMediaBridge::Probe {
namespace {

struct ReadRange {
    std::int64_t begin = 0;
    std::int64_t end = 0;
};

struct BoundedIoState {
    std::FILE* file = nullptr;
    std::int64_t fileSize = 0;
    std::int64_t position = 0;
    std::uint64_t budget = 0;
    std::uint64_t actualReadBytes = 0;
    bool budgetExceeded = false;
    bool readError = false;
    bool syntheticEof = false;
    bool seekable = true;
    std::uint64_t readErrorAfterBytes = 0;
    std::uint64_t syntheticEofAfterBytes = 0;
    std::vector<ReadRange> reads;
};

struct TailPacketEvidence {
    std::uint64_t packetCount = 0;
    std::int64_t firstPts = AV_NOPTS_VALUE;
    std::int64_t previousPts = AV_NOPTS_VALUE;
    std::int64_t maximumEnd = AV_NOPTS_VALUE;
    std::int64_t finalDurationFrames = 0;
    std::int64_t finalPayloadFrames = 0;
    bool allPayloadAligned = true;
    bool allDurationsExact = true;
    bool allDurationPayloadAgree = true;
    bool ptsMonotonic = true;
    bool arithmeticValid = true;
    bool finalDurationPayloadAgree = false;
};

struct SecondaryInput {
    BoundedIoState io;
    AVIOContext* avio = nullptr;
    AVFormatContext* format = nullptr;

    ~SecondaryInput() {
        if (format) {
            avformat_close_input(&format);
        }
        if (avio) {
            av_freep(&avio->buffer);
            avio_context_free(&avio);
        }
        if (io.file) {
            std::fclose(io.file);
        }
    }
};

bool formatListContains(const char* names, std::string_view expected) noexcept {
    if (!names) {
        return false;
    }
    std::string_view remaining(names);
    while (!remaining.empty()) {
        const std::size_t comma = remaining.find(',');
        if (remaining.substr(0, comma) == expected) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(comma + 1);
    }
    return false;
}

int packedPcmBytesPerSample(AVCodecID codecId) noexcept {
    switch (codecId) {
        case AV_CODEC_ID_PCM_S8:
        case AV_CODEC_ID_PCM_U8:
            return 1;
        case AV_CODEC_ID_PCM_S16LE:
        case AV_CODEC_ID_PCM_S16BE:
        case AV_CODEC_ID_PCM_U16LE:
        case AV_CODEC_ID_PCM_U16BE:
            return 2;
        case AV_CODEC_ID_PCM_S24LE:
        case AV_CODEC_ID_PCM_S24BE:
        case AV_CODEC_ID_PCM_U24LE:
        case AV_CODEC_ID_PCM_U24BE:
            return 3;
        case AV_CODEC_ID_PCM_S32LE:
        case AV_CODEC_ID_PCM_S32BE:
        case AV_CODEC_ID_PCM_U32LE:
        case AV_CODEC_ID_PCM_U32BE:
        case AV_CODEC_ID_PCM_F32LE:
        case AV_CODEC_ID_PCM_F32BE:
            return 4;
        case AV_CODEC_ID_PCM_S64LE:
        case AV_CODEC_ID_PCM_S64BE:
        case AV_CODEC_ID_PCM_F64LE:
        case AV_CODEC_ID_PCM_F64BE:
            return 8;
        default:
            return 0;
    }
}

bool checkedAdd(std::int64_t left, std::int64_t right, std::int64_t& result) noexcept {
    if ((right > 0 && left > (std::numeric_limits<std::int64_t>::max)() - right) ||
        (right < 0 && left < (std::numeric_limits<std::int64_t>::min)() - right)) {
        return false;
    }
    result = left + right;
    return true;
}

bool exactNativeFrames(
    std::int64_t units,
    AVRational timeBase,
    int sampleRate,
    std::int64_t& frames) noexcept {
    if (units < 0 || timeBase.num <= 0 || timeBase.den <= 0 || sampleRate <= 0) {
        return false;
    }
    const AVRational sampleTimeBase{1, sampleRate};
    const std::int64_t candidate = av_rescale_q(units, timeBase, sampleTimeBase);
    if (candidate < 0 ||
        av_compare_ts(units, timeBase, candidate, sampleTimeBase) != 0) {
        return false;
    }
    frames = candidate;
    return true;
}

std::uint64_t uniqueReadBytes(std::vector<ReadRange> ranges) {
    if (ranges.empty()) {
        return 0;
    }
    std::sort(ranges.begin(), ranges.end(), [](const ReadRange& left, const ReadRange& right) {
        return left.begin < right.begin;
    });
    std::int64_t begin = ranges.front().begin;
    std::int64_t end = ranges.front().end;
    std::uint64_t total = 0;
    for (std::size_t index = 1; index < ranges.size(); ++index) {
        if (ranges[index].begin <= end) {
            end = std::max(end, ranges[index].end);
        } else {
            total += static_cast<std::uint64_t>(end - begin);
            begin = ranges[index].begin;
            end = ranges[index].end;
        }
    }
    return total + static_cast<std::uint64_t>(end - begin);
}

int readBounded(void* opaque, std::uint8_t* buffer, int requested) {
    auto& state = *static_cast<BoundedIoState*>(opaque);
    if (requested <= 0) {
        return AVERROR(EINVAL);
    }
    if (state.syntheticEofAfterBytes > 0 &&
        state.actualReadBytes >= state.syntheticEofAfterBytes) {
        state.syntheticEof = true;
        return AVERROR_EOF;
    }
    if (state.position >= state.fileSize) {
        return AVERROR_EOF;
    }
    if (state.readErrorAfterBytes > 0 &&
        state.actualReadBytes >= state.readErrorAfterBytes) {
        state.readError = true;
        return AVERROR(EIO);
    }
    if (state.actualReadBytes >= state.budget) {
        state.budgetExceeded = true;
        return AVERROR(ENOBUFS);
    }

    std::uint64_t allowed = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(requested), state.budget - state.actualReadBytes);
    if (state.readErrorAfterBytes > state.actualReadBytes) {
        allowed = std::min(allowed, state.readErrorAfterBytes - state.actualReadBytes);
    }
    if (state.syntheticEofAfterBytes > state.actualReadBytes) {
        allowed = std::min(allowed, state.syntheticEofAfterBytes - state.actualReadBytes);
    }
    if (allowed == 0) {
        state.budgetExceeded = true;
        return AVERROR(ENOBUFS);
    }

    const std::int64_t begin = state.position;
    const std::size_t actual = std::fread(
        buffer, 1, static_cast<std::size_t>(allowed), state.file);
    if (actual == 0) {
        if (std::feof(state.file)) {
            return AVERROR_EOF;
        }
        state.readError = true;
        return AVERROR(errno ? errno : EIO);
    }
    state.position += static_cast<std::int64_t>(actual);
    state.actualReadBytes += actual;
    state.reads.push_back({begin, state.position});
    return static_cast<int>(actual);
}

std::int64_t seekBounded(void* opaque, std::int64_t offset, int whence) {
    auto& state = *static_cast<BoundedIoState*>(opaque);
    if (whence & AVSEEK_SIZE) {
        return state.seekable ? state.fileSize : AVERROR(ENOSYS);
    }
    if (!state.seekable) {
        return AVERROR(ENOSYS);
    }
    const int origin = whence & ~AVSEEK_FORCE;
    if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END) {
        return AVERROR(EINVAL);
    }
    if (_fseeki64(state.file, offset, origin) != 0) {
        return AVERROR(errno ? errno : EIO);
    }
    state.position = _ftelli64(state.file);
    return state.position >= 0 ? state.position : AVERROR(EIO);
}

bool openSecondary(
    SecondaryInput& input,
    const std::string& path,
    const NutBoundedTailProbeOptions& options,
    int& error) {
    const std::filesystem::path nativePath = std::filesystem::u8path(path);
    if (_wfopen_s(&input.io.file, nativePath.c_str(), L"rb") != 0 || !input.io.file) {
        error = AVERROR(errno ? errno : ENOENT);
        return false;
    }
    if (_fseeki64(input.io.file, 0, SEEK_END) != 0) {
        error = AVERROR(errno ? errno : EIO);
        return false;
    }
    input.io.fileSize = _ftelli64(input.io.file);
    if (input.io.fileSize < 0 || _fseeki64(input.io.file, 0, SEEK_SET) != 0) {
        error = AVERROR(EIO);
        return false;
    }
    input.io.budget = options.hardReadBudgetBytes;
    if (options.testHooks) {
        input.io.seekable = !options.testHooks->forceNonSeekable;
        input.io.readErrorAfterBytes = options.testHooks->readErrorAfterBytes;
        input.io.syntheticEofAfterBytes = options.testHooks->syntheticEofAfterBytes;
    }

    constexpr int ioBufferBytes = 32 * 1024;
    auto* buffer = static_cast<std::uint8_t*>(av_malloc(ioBufferBytes));
    if (!buffer) {
        error = AVERROR(ENOMEM);
        return false;
    }
    input.avio = avio_alloc_context(
        buffer, ioBufferBytes, 0, &input.io, readBounded, nullptr, seekBounded);
    if (!input.avio) {
        av_free(buffer);
        error = AVERROR(ENOMEM);
        return false;
    }
    input.avio->seekable = input.io.seekable ? AVIO_SEEKABLE_NORMAL : 0;

    input.format = avformat_alloc_context();
    if (!input.format) {
        error = AVERROR(ENOMEM);
        return false;
    }
    input.format->pb = input.avio;
    input.format->flags |= AVFMT_FLAG_CUSTOM_IO;
    const AVInputFormat* nut = av_find_input_format("nut");
    error = avformat_open_input(&input.format, nullptr, nut, nullptr);
    if (error < 0) {
        return false;
    }
    error = avformat_find_stream_info(input.format, nullptr);
    return error >= 0;
}

TailPacketEvidence readTailPackets(
    AVFormatContext* format,
    int selectedStreamIndex,
    const PcmPacketLayout& layout,
    const NutBoundedTailTestHooks* hooks,
    int& readResult) {
    TailPacketEvidence result;
    const AVStream* stream = format->streams[selectedStreamIndex];
    const AVRational sampleTimeBase{1, layout.sampleRate};
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        readResult = AVERROR(ENOMEM);
        result.arithmeticValid = false;
        return result;
    }
    while ((readResult = av_read_frame(format, packet)) >= 0) {
        if (packet->stream_index == selectedStreamIndex &&
            !(hooks && hooks->suppressSelectedPackets)) {
            const bool aligned = packet->size > 0 && layout.bytesPerFrame > 0 &&
                packet->size % layout.bytesPerFrame == 0 &&
                !(hooks && hooks->forceMisalignedPayload);
            const std::int64_t payloadFrames = aligned
                ? packet->size / layout.bytesPerFrame
                : 0;
            const bool durationPresent = packet->duration > 0 &&
                !(hooks && hooks->forceMissingDuration);
            std::int64_t durationFrames = 0;
            const bool durationExact = durationPresent && exactNativeFrames(
                packet->duration, stream->time_base, layout.sampleRate, durationFrames);
            std::int64_t packetEnd = AV_NOPTS_VALUE;
            if (packet->pts == AV_NOPTS_VALUE || !durationPresent ||
                !checkedAdd(packet->pts, packet->duration, packetEnd)) {
                result.arithmeticValid = false;
            }
            if (result.firstPts == AV_NOPTS_VALUE) {
                result.firstPts = packet->pts;
            }
            if (result.previousPts != AV_NOPTS_VALUE &&
                packet->pts != AV_NOPTS_VALUE && packet->pts < result.previousPts) {
                result.ptsMonotonic = false;
            }
            result.previousPts = packet->pts;
            ++result.packetCount;
            result.allPayloadAligned = result.allPayloadAligned && aligned;
            result.allDurationsExact = result.allDurationsExact && durationExact;
            result.allDurationPayloadAgree = result.allDurationPayloadAgree &&
                aligned && durationExact && durationFrames == payloadFrames;
            if (packetEnd != AV_NOPTS_VALUE &&
                (result.maximumEnd == AV_NOPTS_VALUE || packetEnd >= result.maximumEnd)) {
                result.maximumEnd = packetEnd;
                result.finalDurationFrames = durationFrames;
                result.finalPayloadFrames = payloadFrames;
                result.finalDurationPayloadAgree =
                    aligned && durationExact && durationFrames == payloadFrames;
            }
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    return result;
}

void copyIoDiagnostics(
    NutBoundedTailProbeResult& result,
    const BoundedIoState& io) noexcept {
    result.totalActualReadBytes = io.actualReadBytes;
    result.maximumBudgetOverrunBytes =
        io.actualReadBytes > io.budget ? io.actualReadBytes - io.budget : 0;
}

}  // namespace

PcmPacketLayout deriveUncompressedPcmPacketLayout(
    const AVCodecParameters* codecpar) noexcept {
    PcmPacketLayout result;
    if (!codecpar || codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        result.rejectionReason = "not_audio";
        return result;
    }
    result.sampleRate = codecpar->sample_rate;
    result.channels = codecpar->ch_layout.nb_channels;
    result.bytesPerSample = packedPcmBytesPerSample(codecpar->codec_id);
    if (result.sampleRate <= 0) {
        result.rejectionReason = "unknown_sample_rate";
        return result;
    }
    if (result.channels <= 0 || !av_channel_layout_check(&codecpar->ch_layout)) {
        result.rejectionReason = "unknown_channel_layout";
        return result;
    }
    if (result.bytesPerSample <= 0) {
        result.rejectionReason = "unsupported_pcm_layout";
        return result;
    }
    if (result.channels >
        (std::numeric_limits<std::int64_t>::max)() / result.bytesPerSample) {
        result.rejectionReason = "layout_overflow";
        return result;
    }
    result.bytesPerFrame =
        static_cast<std::int64_t>(result.channels) * result.bytesPerSample;
    if (codecpar->block_align > 0 && codecpar->block_align != result.bytesPerFrame) {
        result.rejectionReason = "block_align_conflict";
        return result;
    }
    result.eligible = true;
    result.rejectionReason = "eligible";
    return result;
}

bool shouldProbeNutBoundedTailAuthority(
    const std::string& path,
    const AVFormatContext* formatContext,
    const AVStream* selectedAudioStream,
    bool strongerSampleExactAuthorityPresent) noexcept {
    if (path.empty() || !formatContext || !formatContext->iformat ||
        !selectedAudioStream || strongerSampleExactAuthorityPresent ||
        !formatListContains(formatContext->iformat->name, "nut") ||
        !formatContext->pb ||
        (formatContext->pb->seekable & AVIO_SEEKABLE_NORMAL) == 0 ||
        !deriveUncompressedPcmPacketLayout(selectedAudioStream->codecpar).eligible) {
        return false;
    }
    try {
        std::error_code error;
        return std::filesystem::is_regular_file(std::filesystem::u8path(path), error) && !error;
    } catch (...) {
        return false;
    }
}

NutBoundedTailProbeResult probeNutBoundedTailAuthority(
    const std::string& path,
    int selectedStreamIndex,
    const NutBoundedTailProbeOptions& options) try {
    NutBoundedTailProbeResult result;
    result.hardReadBudgetBytes = options.hardReadBudgetBytes;
    result.selectedStreamIndex = selectedStreamIndex;
    if (options.hardReadBudgetBytes == 0 || path.empty() || selectedStreamIndex < 0) {
        result.status = NutBoundedTailStatus::Unavailable;
        result.reason = "invalid_request";
        return result;
    }

    SecondaryInput input;
    int error = 0;
    if (!openSecondary(input, path, options, error)) {
        copyIoDiagnostics(result, input.io);
        if (input.io.budgetExceeded) {
            result.status = NutBoundedTailStatus::IoBudgetExceeded;
            result.reason = "budget_exhausted_during_open";
        } else if (input.io.readError || input.io.syntheticEof) {
            result.status = NutBoundedTailStatus::Unavailable;
            result.reason = "open_did_not_reach_complete_media";
        } else {
            result.status = NutBoundedTailStatus::InvalidMedia;
            result.reason = "secondary_open_failed";
        }
        return result;
    }

    if (!input.format->iformat ||
        !formatListContains(input.format->iformat->name, "nut")) {
        result.status = NutBoundedTailStatus::InvalidMedia;
        result.reason = "secondary_format_is_not_nut";
        copyIoDiagnostics(result, input.io);
        return result;
    }
    if (selectedStreamIndex >= static_cast<int>(input.format->nb_streams)) {
        result.status = NutBoundedTailStatus::Unavailable;
        result.reason = "selected_stream_absent";
        copyIoDiagnostics(result, input.io);
        return result;
    }
    AVStream* stream = input.format->streams[selectedStreamIndex];
    const PcmPacketLayout layout = deriveUncompressedPcmPacketLayout(
        stream ? stream->codecpar : nullptr);
    if (!layout.eligible) {
        result.status = NutBoundedTailStatus::Unavailable;
        result.reason = layout.rejectionReason;
        copyIoDiagnostics(result, input.io);
        return result;
    }
    result.sampleRate = layout.sampleRate;
    if (!input.io.seekable) {
        result.status = NutBoundedTailStatus::Unavailable;
        result.reason = "input_not_seekable";
        copyIoDiagnostics(result, input.io);
        return result;
    }

    const bool wholeFileCovered =
        input.io.fileSize >= 0 &&
        uniqueReadBytes(input.io.reads) >= static_cast<std::uint64_t>(input.io.fileSize);
    int indexEntries = avformat_index_get_entries_count(stream);
    if (options.testHooks && options.testHooks->forceNoIndex) {
        indexEntries = 0;
    }
    if (indexEntries == 0 && !wholeFileCovered) {
        result.status = NutBoundedTailStatus::Unavailable;
        result.reason = "usable_index_unavailable";
        copyIoDiagnostics(result, input.io);
        return result;
    }

    std::int64_t seekTarget = 0;
    if (input.format->duration != AV_NOPTS_VALUE && input.format->duration >= 0) {
        seekTarget = av_rescale_q(
            input.format->duration, AV_TIME_BASE_Q, stream->time_base);
    } else if (stream->duration != AV_NOPTS_VALUE && stream->duration >= 0) {
        seekTarget = stream->duration;
    }
    if (options.testHooks && options.testHooks->overrideSeekTarget) {
        seekTarget = options.testHooks->seekTarget;
    }

    int seekResult = 0;
    if (options.testHooks && options.testHooks->forceSeekFailure) {
        seekResult = AVERROR(EIO);
    } else {
        seekResult = avformat_seek_file(
            input.format, selectedStreamIndex,
            (std::numeric_limits<std::int64_t>::min)(), seekTarget,
            (std::numeric_limits<std::int64_t>::max)(), AVSEEK_FLAG_BACKWARD);
    }
    result.timestampSeekSucceeded = seekResult >= 0;
    if (input.io.budgetExceeded) {
        result.status = NutBoundedTailStatus::IoBudgetExceeded;
        result.reason = "budget_exhausted_during_timestamp_seek";
        copyIoDiagnostics(result, input.io);
        return result;
    }
    if (seekResult < 0) {
        result.status = NutBoundedTailStatus::Unavailable;
        result.reason = "timestamp_seek_failed";
        copyIoDiagnostics(result, input.io);
        return result;
    }

    int readResult = 0;
    const TailPacketEvidence evidence = readTailPackets(
        input.format, selectedStreamIndex, layout, options.testHooks, readResult);
    result.targetPacketsObserved = evidence.packetCount;
    result.payloadAligned = evidence.allPayloadAligned;
    result.durationPayloadAgree = evidence.finalDurationPayloadAgree;
    result.reachedPhysicalEof =
        readResult == AVERROR_EOF && !input.io.syntheticEof &&
        input.io.position >= input.io.fileSize;
    copyIoDiagnostics(result, input.io);

    if (input.io.budgetExceeded) {
        result.status = NutBoundedTailStatus::IoBudgetExceeded;
        result.reason = "budget_exhausted_during_tail_read";
        return result;
    }
    if (input.io.readError || input.io.syntheticEof || !result.reachedPhysicalEof) {
        result.status = NutBoundedTailStatus::Unavailable;
        result.reason = "tail_read_did_not_reach_physical_eof";
        return result;
    }
    if (evidence.packetCount == 0) {
        result.status = NutBoundedTailStatus::Unavailable;
        result.reason = "selected_stream_packet_absent_after_seek";
        return result;
    }
    if (!evidence.allPayloadAligned || !evidence.allDurationsExact ||
        !evidence.allDurationPayloadAgree ||
        !evidence.finalDurationPayloadAgree || !evidence.ptsMonotonic ||
        !evidence.arithmeticValid || evidence.maximumEnd == AV_NOPTS_VALUE) {
        result.status = NutBoundedTailStatus::Unavailable;
        result.reason = "packet_end_evidence_incomplete";
        return result;
    }

    const std::int64_t authoritativeStart = stream->start_time;
    if (authoritativeStart == AV_NOPTS_VALUE || evidence.maximumEnd < authoritativeStart) {
        result.status = NutBoundedTailStatus::Unavailable;
        result.reason = "authoritative_stream_start_unavailable";
        return result;
    }
    const std::int64_t spanUnits = evidence.maximumEnd - authoritativeStart;
    std::int64_t presentationFrames = 0;
    if (!exactNativeFrames(
            spanUnits, stream->time_base, layout.sampleRate, presentationFrames) ||
        presentationFrames <= 0) {
        result.status = NutBoundedTailStatus::Unavailable;
        result.reason = "presentation_span_not_sample_exact";
        return result;
    }
    result.status = NutBoundedTailStatus::Exact;
    result.presentationFrames = static_cast<std::uint64_t>(presentationFrames);
    result.reason = "selected_stream_half_open_end_proven";
    return result;
} catch (...) {
    NutBoundedTailProbeResult result;
    result.status = NutBoundedTailStatus::Unavailable;
    result.hardReadBudgetBytes = options.hardReadBudgetBytes;
    result.selectedStreamIndex = selectedStreamIndex;
    result.reason = "secondary_probe_exception";
    return result;
}

const char* nutBoundedTailStatusName(NutBoundedTailStatus status) noexcept {
    switch (status) {
        case NutBoundedTailStatus::Exact:
            return "exact";
        case NutBoundedTailStatus::Unavailable:
            return "unavailable";
        case NutBoundedTailStatus::IoBudgetExceeded:
            return "io_budget_exceeded";
        case NutBoundedTailStatus::Conflict:
            return "conflict";
        case NutBoundedTailStatus::InvalidMedia:
            return "invalid_media";
    }
    return "unavailable";
}

}  // namespace AveMediaBridge::Probe
