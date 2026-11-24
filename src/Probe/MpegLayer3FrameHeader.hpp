#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace AveMediaBridge::Probe {
struct MpegFrameHeader {
    bool valid = false;
    int version = 0;
    int layer = 0;
    int sampleRate = 0;
    int bitrateKbps = 0;
    int padding = 0;
    int channels = 0;
    bool crcPresent = false;
    std::uint64_t samplesPerFrame = 0;
    std::uint64_t frameSizeBytes = 0;
};

inline MpegFrameHeader parseMpegFrameHeader(const std::uint8_t* bytes, std::size_t size) noexcept {
    MpegFrameHeader result;
    if (!bytes || size < 4) {
        return result;
    }
    const std::uint32_t value = (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) |
        (std::uint32_t{bytes[2]} << 8) | bytes[3];
    if ((value & 0xffe00000U) != 0xffe00000U) {
        return result;
    }
    const int versionBits = static_cast<int>((value >> 19U) & 0x03U);
    const int layerBits = static_cast<int>((value >> 17U) & 0x03U);
    const int bitrateIndex = static_cast<int>((value >> 12U) & 0x0fU);
    const int sampleRateIndex = static_cast<int>((value >> 10U) & 0x03U);
    if (versionBits == 1 || layerBits != 1 || bitrateIndex == 0 ||
        bitrateIndex == 15 || sampleRateIndex == 3) {
        return result;
    }

    static constexpr std::array<int, 15> kMpeg1Layer3Bitrates{
        0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    static constexpr std::array<int, 15> kMpeg2Layer3Bitrates{
        0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    static constexpr std::array<int, 3> kMpeg1Rates{44100, 48000, 32000};
    static constexpr std::array<int, 3> kMpeg2Rates{22050, 24000, 16000};
    static constexpr std::array<int, 3> kMpeg25Rates{11025, 12000, 8000};

    result.version = versionBits == 3 ? 1 : (versionBits == 2 ? 2 : 25);
    result.layer = 3;
    result.sampleRate = result.version == 1
        ? kMpeg1Rates[sampleRateIndex]
        : result.version == 2
            ? kMpeg2Rates[sampleRateIndex]
            : kMpeg25Rates[sampleRateIndex];
    result.bitrateKbps = result.version == 1
        ? kMpeg1Layer3Bitrates[bitrateIndex]
        : kMpeg2Layer3Bitrates[bitrateIndex];
    result.padding = static_cast<int>((value >> 9U) & 1U);
    result.channels = ((value >> 6U) & 0x03U) == 3 ? 1 : 2;
    result.crcPresent = ((value >> 16U) & 1U) == 0;
    result.samplesPerFrame = result.version == 1 ? 1152 : 576;
    const std::uint64_t coefficient = result.version == 1 ? 144 : 72;
    result.frameSizeBytes =
        coefficient * static_cast<std::uint64_t>(result.bitrateKbps) * 1000U /
        static_cast<std::uint64_t>(result.sampleRate) +
        static_cast<std::uint64_t>(result.padding);
    result.valid = result.frameSizeBytes >= 4;
    return result;
}

}  // namespace AveMediaBridge::Probe
