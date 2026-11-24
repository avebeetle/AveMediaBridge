#pragma once

#include "../Ffmpeg/FfmpegHeaders.hpp"
#include <string>

namespace AveMediaBridge::Decode {

void configureElementaryDecode(const AVFormatContext& input, AVCodecContext& decoder);
bool rejectDamagedPacket(const AVCodecContext& decoder, int error, std::string& message);

int copyDecodedFrameLayout(
    AVChannelLayout* dst,
    const AVFrame* frame,
    const AVCodecContext* decoder);

}  // namespace AveMediaBridge::Decode
