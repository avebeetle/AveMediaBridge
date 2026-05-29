#pragma once

#include <string>

#include "AveMediaBridge/Core/AudioImportResult.hpp"
#include "AveMediaBridge/Core/AudioBufferF32.hpp"

namespace AveMediaBridge {

class AveMediaBridge {
public:
    AudioImportResult importAudio(const std::string& path);
    bool exportAudioAsPcm16Wav(const AudioBufferF32& audio, const std::string& outputPath, std::string& error);
    bool transformToWav(const std::string& inputPath, const std::string& outputWavPath, float gain, std::string& error);
};

}  // namespace AveMediaBridge
