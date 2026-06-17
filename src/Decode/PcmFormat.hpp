#pragma once

#include "../Ffmpeg/FfmpegHeaders.hpp"

#include <string>

namespace AveMediaBridge::Decode {

std::string sampleFormatName(AVSampleFormat format);

}  // namespace AveMediaBridge::Decode
