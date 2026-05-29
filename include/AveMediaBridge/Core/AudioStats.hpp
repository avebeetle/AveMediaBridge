#pragma once

#include "AveMediaBridge/Core/AudioBufferF32.hpp"

namespace AveMediaBridge {

struct AudioStats {
    double rms = 0.0;
    float peakAbs = 0.0f;
    float minSample = 0.0f;
    float maxSample = 0.0f;
    bool clippingRisk = false;
};

AudioStats computeAudioStats(const AudioBufferF32& audio);

}  // namespace AveMediaBridge
