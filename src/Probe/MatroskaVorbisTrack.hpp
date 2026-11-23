#pragma once

#include "../Ffmpeg/FfmpegHeaders.hpp"

#include <cstdint>
#include <optional>

namespace AveMediaBridge::Probe {

struct MatroskaVorbisTrackExtent {
    std::int64_t exactFrames = 0;
    std::int64_t estimatedFrames = 0;
};

// nullopt = other profile, zero = this profile with no usable selected-track estimate.
std::optional<std::int64_t> estimateMatroskaVorbisTrack(const AVFormatContext* context, const AVStream* audio);

// Final operation on this probe's demuxer. Does not touch the import reader.
MatroskaVorbisTrackExtent probeMatroskaVorbisTrack(AVFormatContext* context, const AVStream* audio);

} // namespace AveMediaBridge::Probe
