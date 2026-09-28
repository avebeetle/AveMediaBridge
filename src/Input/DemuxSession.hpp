#pragma once

#include "Input/MediaInputSource.hpp"
#include "Ffmpeg/FfmpegDeleters.hpp"

#include <cstdint>
#include <memory>

namespace AveMediaBridge::Input {
class StableAvioInput;

struct DemuxOpenOptions {
    bool useProbeLimits = false;
    int64_t probeSizeBytes = 0;
    int64_t analyzeDurationUs = 0;
};

class DemuxSession final {
public:
    static int open(const MediaInputSource& source, DemuxOpenOptions options,
        std::unique_ptr<DemuxSession>& out) noexcept;
    ~DemuxSession() noexcept;
    DemuxSession(const DemuxSession&) = delete;
    DemuxSession& operator=(const DemuxSession&) = delete;

    AVFormatContext* get() const noexcept;
    int inputError() const noexcept;

private:
    DemuxSession() = default;
    Ffmpeg::UniqueAVFormatContext pathContext_;
    std::unique_ptr<StableAvioInput> stableContext_;
};
}
