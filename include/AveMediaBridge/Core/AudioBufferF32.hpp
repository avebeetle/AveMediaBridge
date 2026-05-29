#pragma once

#include <cstdint>
#include <vector>

namespace AveMediaBridge {

struct AudioBufferF32 {
    int sampleRate = 0;
    int channels = 0;
    std::vector<float> samples;

    bool empty() const;
    std::int64_t frameCount() const;
    std::int64_t sampleCount() const;
    double durationSeconds() const;
    bool isConsistent() const;
    void clear();
};

}  // namespace AveMediaBridge
