#include "AveMediaBridge/Core/AudioStats.hpp"

#include <cmath>
#include <limits>

namespace AveMediaBridge {

AudioStats computeAudioStats(const AudioBufferF32& audio) {
    AudioStats stats;
    if (audio.samples.empty()) {
        return stats;
    }

    long double sumSquares = 0.0L;
    float minSample = std::numeric_limits<float>::infinity();
    float maxSample = -std::numeric_limits<float>::infinity();
    float peak = 0.0f;

    for (const float sample : audio.samples) {
        sumSquares += static_cast<long double>(sample) * static_cast<long double>(sample);
        if (sample < minSample) {
            minSample = sample;
        }
        if (sample > maxSample) {
            maxSample = sample;
        }

        const float absSample = std::fabs(sample);
        if (absSample > peak) {
            peak = absSample;
        }
    }

    stats.rms = static_cast<double>(std::sqrt(sumSquares / static_cast<long double>(audio.samples.size())));
    stats.peakAbs = peak;
    stats.minSample = minSample;
    stats.maxSample = maxSample;
    stats.clippingRisk = peak > 1.0f;
    return stats;
}

}  // namespace AveMediaBridge
