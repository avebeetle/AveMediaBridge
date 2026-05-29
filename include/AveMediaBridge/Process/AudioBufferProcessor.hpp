#pragma once

#include "AveMediaBridge/Core/AudioBufferF32.hpp"

namespace AveMediaBridge {

class AudioBufferProcessor {
public:
    static void applyGain(AudioBufferF32& audio, float gain);
    static void normalizePeak(AudioBufferF32& audio, float targetPeak);
    static bool makeMonoAverage(const AudioBufferF32& input, AudioBufferF32& output);
};

}  // namespace AveMediaBridge
