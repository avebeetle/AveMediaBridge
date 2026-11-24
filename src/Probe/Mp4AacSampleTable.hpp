#pragma once

#include "../Ffmpeg/FfmpegHeaders.hpp"
#include "../Input/MediaInputSource.hpp"

#include <cstdint>
#include <functional>

namespace AveMediaBridge::Probe {

struct Mp4AacIndexResult {
    std::uint64_t frames = 0;
    std::uint64_t physicalFrames = 0; // after priming, before presentation-tail trimming
    std::uint64_t bytesRead = 0;
    const char* reason = "not_eligible";
};

// A positive frame count is the selected track's presentation length. Decoding
// and comparing actual output at EOF remain the caller's responsibilities.
Mp4AacIndexResult probeMp4AacSampleTable(const Input::MediaInputSource& source,
                                         const AVFormatContext* format, const AVStream* audio,
                                         const std::function<bool()>& canceled = {});

} // namespace AveMediaBridge::Probe
