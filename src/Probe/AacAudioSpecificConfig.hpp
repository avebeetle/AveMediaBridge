#pragma once

#include <cstddef>
#include <cstdint>

namespace AveMediaBridge::Probe::MatroskaAacDetail {

struct AacConfig {
    bool valid = false;
    bool supported = false;
    int audioObjectType = 0;
    int sampleRate = 0;
    int channelConfiguration = 0;
    int frameLengthFlag = -1;
    int samplesPerAccessUnit = 0;
    const char* reason = "invalid";
};

AacConfig parseAacAudioSpecificConfig(
    const std::uint8_t* data,
    std::size_t size) noexcept;

}  // namespace AveMediaBridge::Probe::MatroskaAacDetail
