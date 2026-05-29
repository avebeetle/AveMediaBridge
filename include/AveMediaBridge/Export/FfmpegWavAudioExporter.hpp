#pragma once

#include <string>

#include "AveMediaBridge/Core/AudioBufferF32.hpp"

namespace AveMediaBridge {

class FfmpegWavAudioExporter {
public:
    bool exportPcm16Wav(const AudioBufferF32& audio, const std::string& outputPath, std::string& error);
};

}  // namespace AveMediaBridge
