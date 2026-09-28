#include "Input/StableAvioInput.hpp"

#include "Input/StableInputContract.hpp"
#include "Input/DemuxSession.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/dict.h>
}

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <new>

namespace AveMediaBridge::Input {
namespace {
constexpr int kAvioBufferBytes = 65536;

int cancelResult(const AMBI_SourceV1& source) noexcept {
    try {
        const AMBI_Status status = source.checkCancel(source.user);
        if (status == AMBI_CANCELED) return AVERROR_EXIT;
        return status == AMBI_OK ? 0 : AVERROR(EIO);
    } catch (...) {
        return AVERROR(EIO);
    }
}
}

int StableAvioInput::create(const AMBI_SourceV1& source,
    std::unique_ptr<StableAvioInput>& out) noexcept {
    out.reset();
    if (validateSource(&source) != AMBI_OK) return AVERROR(EINVAL);

    std::unique_ptr<StableAvioInput> input(new (std::nothrow) StableAvioInput(source));
    if (!input) return AVERROR(ENOMEM);
    auto* buffer = static_cast<uint8_t*>(av_malloc(kAvioBufferBytes));
    if (!buffer) return AVERROR(ENOMEM);
    input->io_ = avio_alloc_context(buffer, kAvioBufferBytes, 0, input.get(),
        &StableAvioInput::readPacket, nullptr, &StableAvioInput::seekPacket);
    if (!input->io_) {
        av_free(buffer);
        return AVERROR(ENOMEM);
    }
    input->io_->seekable = AVIO_SEEKABLE_NORMAL;
    out = std::move(input);
    return 0;
}

StableAvioInput::~StableAvioInput() noexcept {
    if (format_) avformat_close_input(&format_);
    if (io_) {
        // FFmpeg may replace this buffer during probing; free its current one.
        av_freep(&io_->buffer);
        avio_context_free(&io_);
    }
}

int StableAvioInput::open(const AVInputFormat* forcedFormat) noexcept {
    return open(forcedFormat, {});
}

int StableAvioInput::checkCancel() noexcept {
    if (terminalReadError_ < 0) return terminalReadError_;
    const int canceled = cancelResult(source_);
    if (canceled < 0) terminalReadError_ = canceled;
    return canceled;
}

int StableAvioInput::interrupt(void* opaque) noexcept {
    if (!opaque) return 1;
    return static_cast<StableAvioInput*>(opaque)->checkCancel() < 0 ? 1 : 0;
}

int StableAvioInput::open(const AVInputFormat* forcedFormat,
    DemuxOpenOptions options) noexcept {
    if (opened_) return AVERROR(EINVAL);
    opened_ = true;
    if (!forcedFormat || (forcedFormat->flags & AVFMT_NOFILE)) return AVERROR(EINVAL);
    if (options.useProbeLimits &&
        (options.probeSizeBytes <= 0 || options.analyzeDurationUs <= 0))
        return AVERROR(EINVAL);
    const int canceled = checkCancel();
    if (canceled < 0) return canceled;

    format_ = avformat_alloc_context();
    if (!format_) return AVERROR(ENOMEM);
    format_->pb = io_;
    format_->flags |= AVFMT_FLAG_CUSTOM_IO;
    format_->io_open = &StableAvioInput::denySecondary;
    format_->interrupt_callback = {&StableAvioInput::interrupt, this};
    AVDictionary* dictionary = nullptr;
    if (options.useProbeLimits) {
        format_->probesize = options.probeSizeBytes;
        format_->max_analyze_duration = options.analyzeDurationUs;
        if (av_dict_set_int(&dictionary, "probesize", options.probeSizeBytes, 0) < 0 ||
            av_dict_set_int(&dictionary, "analyzeduration", options.analyzeDurationUs, 0) < 0) {
            av_dict_free(&dictionary);
            return AVERROR(ENOMEM);
        }
    }
    // No URL is supplied. FFmpeg consumes only this custom AVIO context.
    const int result = avformat_open_input(&format_, nullptr, forcedFormat,
        options.useProbeLimits ? &dictionary : nullptr);
    av_dict_free(&dictionary);
    return terminalReadError_ < 0 ? terminalReadError_ : result;
}

int StableAvioInput::readPacket(void* opaque, uint8_t* destination,
    int requested) noexcept {
    if (!opaque) return AVERROR(EINVAL);
    auto& input = *static_cast<StableAvioInput*>(opaque);
    if (input.terminalReadError_ < 0) return input.terminalReadError_;
    if (!destination || requested <= 0)
        return input.terminalReadError_ = AVERROR(EINVAL);
    const int canceled = input.checkCancel();
    if (canceled < 0) return canceled;
    if (input.cursor_ == static_cast<int64_t>(input.source_.byteSize)) return AVERROR_EOF;

    const uint32_t bounded = static_cast<uint32_t>(
        std::min<int>(requested, static_cast<int>(AMBI_MAX_READ_BYTES)));
    uint32_t actual = 0;
    const AMBI_Status status = readSource(input.source_,
        static_cast<uint64_t>(input.cursor_), destination, bounded, actual);
    if (status == AMBI_CANCELED)
        return input.terminalReadError_ = AVERROR_EXIT;
    if (status == AMBI_INVALID_ARGUMENT)
        return input.terminalReadError_ = AVERROR(EINVAL);
    if (status != AMBI_OK && status != AMBI_EOF)
        return input.terminalReadError_ = AVERROR(EIO);
    if (actual == 0) {
        if (status == AMBI_EOF) return AVERROR_EOF;
        return input.terminalReadError_ = AVERROR(EIO);
    }
    input.cursor_ += actual;
    return static_cast<int>(actual);
}

int64_t StableAvioInput::seekPacket(void* opaque, int64_t offset,
    int whence) noexcept {
    if (!opaque) return AVERROR(EINVAL);
    auto& input = *static_cast<StableAvioInput*>(opaque);
    const int canceled = input.checkCancel();
    if (canceled < 0) return canceled;
    if (whence == AVSEEK_SIZE || whence == (AVSEEK_SIZE | AVSEEK_FORCE))
        return static_cast<int64_t>(input.source_.byteSize);
    const int origin = whence & ~AVSEEK_FORCE;
    int64_t base = 0;
    if (origin == SEEK_CUR) base = input.cursor_;
    else if (origin == SEEK_END) base = static_cast<int64_t>(input.source_.byteSize);
    else if (origin != SEEK_SET) return AVERROR(EINVAL);

    if ((offset > 0 && base > std::numeric_limits<int64_t>::max() - offset) ||
        (offset < 0 && offset < -base)) return AVERROR(EINVAL);
    const int64_t position = base + offset;
    if (position < 0 || static_cast<uint64_t>(position) > input.source_.byteSize)
        return AVERROR(EINVAL);
    input.cursor_ = position;
    return position;
}

int StableAvioInput::denySecondary(AVFormatContext*, AVIOContext** secondary,
    const char*, int, AVDictionary**) noexcept {
    if (secondary) *secondary = nullptr;
    return AVERROR(EACCES);
}
}
