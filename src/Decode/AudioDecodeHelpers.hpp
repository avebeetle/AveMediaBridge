#pragma once

#include "../Ffmpeg/FfmpegHeaders.hpp"

namespace AveMediaBridge::Decode {

int copyDecodedFrameLayout(
    AVChannelLayout* dst,
    const AVFrame* frame,
    const AVCodecContext* decoder);

}  // namespace AveMediaBridge::Decode
