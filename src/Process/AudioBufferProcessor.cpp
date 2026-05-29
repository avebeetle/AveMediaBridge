#include "AveMediaBridge/Process/AudioBufferProcessor.hpp"

#include <algorithm>
#include <cmath>

namespace AveMediaBridge {

void AudioBufferProcessor::applyGain(AudioBufferF32& audio, float gain) {
    for (float& sample : audio.samples) {
        sample *= gain;
    }
}

void AudioBufferProcessor::normalizePeak(AudioBufferF32& audio, float targetPeak) {
    if (audio.samples.empty() || targetPeak <= 0.0f) {
        return;
    }

    float peak = 0.0f;
    for (const float sample : audio.samples) {
        peak = std::max(peak, std::fabs(sample));
    }

    if (peak <= 0.0f) {
        return;
    }

    applyGain(audio, targetPeak / peak);
}

bool AudioBufferProcessor::makeMonoAverage(const AudioBufferF32& input, AudioBufferF32& output) {
    output.clear();

    if (!input.isConsistent() || input.sampleRate <= 0 || input.channels <= 0) {
        return false;
    }

    output.sampleRate = input.sampleRate;
    output.channels = 1;
    output.samples.resize(static_cast<std::size_t>(input.frameCount()));

    if (input.channels == 1) {
        output.samples = input.samples;
        return true;
    }

    const std::size_t channels = static_cast<std::size_t>(input.channels);
    for (std::size_t frame = 0; frame < output.samples.size(); ++frame) {
        float sum = 0.0f;
        const std::size_t base = frame * channels;
        for (std::size_t ch = 0; ch < channels; ++ch) {
            sum += input.samples[base + ch];
        }
        output.samples[frame] = sum / static_cast<float>(channels);
    }

    return true;
}

}  // namespace AveMediaBridge
