#pragma once

#include "../Ffmpeg/FfmpegHeaders.hpp"
#include "../Input/MediaInputSource.hpp"
#include "PresentationBudgetPolicy.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace AveMediaBridge::Probe {

struct MovPcmRange {
    std::uint64_t offset;
    std::uint64_t frames;
};

struct MovPcmEdit {
    unsigned bytesPerFrame = 0;
    std::vector<MovPcmRange> ranges;
};

struct Mp4AudioIndexResult {
    MovPcmEdit pcmEdit;
    std::uint64_t frames = 0;
    std::uint64_t physicalFrames = 0; // after priming, before presentation-tail trimming
    PresentationTotalSource source = PresentationTotalSource::None;
    std::uint64_t bytesRead = 0;
    const char* reason = "not_eligible";
};

// A positive frame count is the selected track's presentation length. Decoding
// and comparing actual output at EOF remain the caller's responsibilities.
Mp4AudioIndexResult probeMp4AudioSampleTable(const Input::MediaInputSource& source,
                                         const AVFormatContext* format, const AVStream* audio,
                                         const std::function<bool()>& canceled = {});

} // namespace AveMediaBridge::Probe
