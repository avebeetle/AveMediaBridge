#pragma once

#include "../Ffmpeg/FfmpegHeaders.hpp"

#include <cstdint>

namespace AveMediaBridge::Probe {

enum class FlacStreamInfoStatus {
    Valid,
    UnsupportedCodec,
    MissingExtradata,
    TruncatedExtradata,
    UnsupportedLayout,
    MalformedStreamInfo,
    SampleRateMismatch,
    ChannelCountMismatch,
    TotalSamplesUnknown
};

struct FlacStreamInfoResult {
    FlacStreamInfoStatus status = FlacStreamInfoStatus::MissingExtradata;
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    std::uint64_t totalSamples = 0;

    bool valid() const noexcept { return status == FlacStreamInfoStatus::Valid; }
};

FlacStreamInfoResult parseFlacStreamInfo(
    const AVCodecParameters* codecpar) noexcept;

const char* flacStreamInfoStatusName(FlacStreamInfoStatus status) noexcept;

}  // namespace AveMediaBridge::Probe
