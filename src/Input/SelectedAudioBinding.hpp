#pragma once

#include "MediaInputSource.hpp"
#include "../Ffmpeg/FfmpegHeaders.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace AveMediaBridge::Input {
struct SelectedAudioBinding {
    std::array<std::uint8_t, 16> sourceToken{};
    std::uint64_t byteSize = 0;
    std::string demuxerName;
    int streamIndex = -1;
    int trackId = -1;
    int codecId = 0;
    int sampleRate = 0;
    int channels = 0;
    std::string channelLayout;
    int timeBaseNumerator = 0;
    int timeBaseDenominator = 0;
    std::vector<std::uint8_t> extradata;
};

SelectedAudioBinding bindSelectedAudio(
    const MediaInputSource& source, const AVFormatContext* context, int streamIndex);
bool matchesSelectedAudio(
    const SelectedAudioBinding& expected, const MediaInputSource& source,
    const AVFormatContext* context, int streamIndex);
}
