#include "Input/DemuxSession.hpp"

#include "Input/StableAvioInput.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

#include <cerrno>
#include <new>

namespace AveMediaBridge::Input {
DemuxSession::~DemuxSession() noexcept = default;

int DemuxSession::open(const MediaInputSource& source, DemuxOpenOptions options,
    std::unique_ptr<DemuxSession>& out) noexcept {
    out.reset();
    std::unique_ptr<DemuxSession> session(new (std::nothrow) DemuxSession());
    if (!session) return AVERROR(ENOMEM);
    if (source.isStable()) {
        const AMBI_SourceV1* stable = source.stableSource();
        if (!stable) return AVERROR(EINVAL);
        int result = StableAvioInput::create(*stable, session->stableContext_);
        if (result < 0) return result;
        const AVInputFormat* mov = av_find_input_format("mov");
        if (!mov) return AVERROR_DEMUXER_NOT_FOUND;
        result = session->stableContext_->open(mov, options);
        if (result < 0) return result;
    } else {
        const std::string* path = source.legacyPath();
        if (!path || path->empty()) return AVERROR(EINVAL);
        AVFormatContext* raw = nullptr;
        const int result = avformat_open_input(&raw, path->c_str(), nullptr, nullptr);
        session->pathContext_.reset(raw);
        if (result < 0) return result;
    }
    out = std::move(session);
    return 0;
}

AVFormatContext* DemuxSession::get() const noexcept {
    return stableContext_ ? stableContext_->format() : pathContext_.get();
}
int DemuxSession::inputError() const noexcept {
    return stableContext_ ? stableContext_->terminalError() : 0;
}
}
