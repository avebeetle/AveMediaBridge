#include "AveMediaBridge/Core/AudioBufferF32.hpp"

namespace AveMediaBridge {

bool AudioBufferF32::empty() const {
    return samples.empty();
}

std::int64_t AudioBufferF32::frameCount() const {
    if (channels <= 0) {
        return 0;
    }
    return static_cast<std::int64_t>(samples.size() / static_cast<std::size_t>(channels));
}

std::int64_t AudioBufferF32::sampleCount() const {
    return static_cast<std::int64_t>(samples.size());
}

double AudioBufferF32::durationSeconds() const {
    if (sampleRate <= 0) {
        return 0.0;
    }
    return static_cast<double>(frameCount()) / static_cast<double>(sampleRate);
}

bool AudioBufferF32::isConsistent() const {
    if (sampleRate <= 0 || channels <= 0) {
        return samples.empty();
    }
    return samples.size() % static_cast<std::size_t>(channels) == 0;
}

void AudioBufferF32::clear() {
    sampleRate = 0;
    channels = 0;
    samples.clear();
}

}  // namespace AveMediaBridge
