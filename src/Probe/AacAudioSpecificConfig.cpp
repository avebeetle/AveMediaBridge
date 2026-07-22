#include "AacAudioSpecificConfig.hpp"

#include <array>

namespace AveMediaBridge::Probe::MatroskaAacDetail {
namespace {

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size) : data_(data), bits_(size * 8U) {}

    bool read(unsigned count, std::uint32_t& value) noexcept {
        if (!data_ || count > 32 || cursor_ > bits_ || count > bits_ - cursor_) {
            return false;
        }
        value = 0;
        for (unsigned index = 0; index < count; ++index) {
            const std::size_t bit = cursor_++;
            value = (value << 1) |
                ((data_[bit / 8U] >> (7U - static_cast<unsigned>(bit % 8U))) & 1U);
        }
        return true;
    }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t bits_ = 0;
    std::size_t cursor_ = 0;
};

int readAudioObjectType(BitReader& reader) noexcept {
    std::uint32_t value = 0;
    if (!reader.read(5, value)) {
        return 0;
    }
    if (value != 31) {
        return static_cast<int>(value);
    }
    std::uint32_t extension = 0;
    return reader.read(6, extension) ? 32 + static_cast<int>(extension) : 0;
}

int readSampleRate(BitReader& reader) noexcept {
    static constexpr std::array<int, 13> rates{
        96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350};
    std::uint32_t index = 0;
    if (!reader.read(4, index)) {
        return 0;
    }
    if (index == 15) {
        std::uint32_t explicitRate = 0;
        return reader.read(24, explicitRate) ? static_cast<int>(explicitRate) : 0;
    }
    return index < rates.size() ? rates[index] : 0;
}

}  // namespace

AacConfig parseAacAudioSpecificConfig(
    const std::uint8_t* data,
    std::size_t size) noexcept {
    AacConfig result;
    BitReader reader(data, size);
    result.audioObjectType = readAudioObjectType(reader);
    result.sampleRate = readSampleRate(reader);
    std::uint32_t channels = 0;
    if (result.audioObjectType == 0 || result.sampleRate <= 0 ||
        !reader.read(4, channels)) {
        return result;
    }
    result.channelConfiguration = static_cast<int>(channels);
    result.valid = true;
    if (result.audioObjectType == 5 || result.audioObjectType == 29) {
        result.reason = "explicit_sbr_or_ps_unsupported";
        return result;
    }
    if (result.audioObjectType != 2) {
        result.reason = "aac_profile_unsupported";
        return result;
    }
    std::uint32_t frameLength = 0;
    std::uint32_t dependsOnCoreCoder = 0;
    std::uint32_t extensionFlag = 0;
    if (!reader.read(1, frameLength) ||
        !reader.read(1, dependsOnCoreCoder)) {
        result.reason = "truncated_ga_specific_config";
        return result;
    }
    if (dependsOnCoreCoder != 0) {
        std::uint32_t coreDelay = 0;
        if (!reader.read(14, coreDelay)) {
            result.reason = "truncated_core_coder_delay";
            return result;
        }
    }
    if (!reader.read(1, extensionFlag)) {
        result.reason = "truncated_ga_specific_config";
        return result;
    }
    result.frameLengthFlag = static_cast<int>(frameLength);
    result.samplesPerAccessUnit = frameLength ? 960 : 1024;
    result.supported = result.channelConfiguration > 0;
    result.reason = result.supported ? "aac_lc_ga_specific_config" : "pce_unsupported";
    return result;
}

}  // namespace AveMediaBridge::Probe::MatroskaAacDetail
