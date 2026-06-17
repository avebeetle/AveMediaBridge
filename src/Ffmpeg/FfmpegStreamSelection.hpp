#pragma once

#include "FfmpegHeaders.hpp"

namespace AveMediaBridge::Ffmpeg {

struct AudioStreamSelection {
    int streamIndex = -1;
    const AVCodec* decoder = nullptr;
    int bestStreamResult = AVERROR_STREAM_NOT_FOUND;
    bool usedFirstAudioFallback = false;
};

bool isAudioStream(const AVStream* stream) noexcept;
int findFirstAudioStream(const AVFormatContext* formatContext) noexcept;
AudioStreamSelection selectBestAudioStreamStrict(AVFormatContext* formatContext) noexcept;
AudioStreamSelection selectBestAudioStreamWithFirstAudioFallback(
    AVFormatContext* formatContext) noexcept;

}  // namespace AveMediaBridge::Ffmpeg

