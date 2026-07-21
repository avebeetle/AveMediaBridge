#include "FlacStreamInfo.hpp"

#include <cstddef>

namespace AveMediaBridge::Probe {
namespace {

constexpr int kStreamInfoBytes = 34;
constexpr std::uint16_t kMinimumFlacBlockSize = 16;

std::uint16_t readBigEndian16(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[0]) << 8U) |
        static_cast<std::uint16_t>(bytes[1]));
}

std::uint32_t readBigEndian32(const std::uint8_t* bytes) noexcept {
    return
        (static_cast<std::uint32_t>(bytes[0]) << 24U) |
        (static_cast<std::uint32_t>(bytes[1]) << 16U) |
        (static_cast<std::uint32_t>(bytes[2]) << 8U) |
        static_cast<std::uint32_t>(bytes[3]);
}

}  // namespace

FlacStreamInfoResult parseFlacStreamInfo(
    const AVCodecParameters* codecpar) noexcept {
    FlacStreamInfoResult result;
    if (!codecpar || codecpar->codec_id != AV_CODEC_ID_FLAC) {
        result.status = FlacStreamInfoStatus::UnsupportedCodec;
        return result;
    }
    if (!codecpar->extradata || codecpar->extradata_size <= 0) {
        result.status = FlacStreamInfoStatus::MissingExtradata;
        return result;
    }
    if (codecpar->extradata_size < kStreamInfoBytes) {
        result.status = FlacStreamInfoStatus::TruncatedExtradata;
        return result;
    }
    // FFmpeg exposes the FLAC STREAMINFO payload itself in codecpar extradata.
    // Container framing such as "fLaC" plus a metadata-block header is not this
    // public layout and must not be guessed here.
    if (codecpar->extradata_size != kStreamInfoBytes) {
        result.status = FlacStreamInfoStatus::UnsupportedLayout;
        return result;
    }

    const std::uint8_t* bytes = codecpar->extradata;
    const std::uint16_t minimumBlockSize = readBigEndian16(bytes);
    const std::uint16_t maximumBlockSize = readBigEndian16(bytes + 2);
    if (minimumBlockSize < kMinimumFlacBlockSize ||
        maximumBlockSize < minimumBlockSize) {
        result.status = FlacStreamInfoStatus::MalformedStreamInfo;
        return result;
    }

    result.sampleRate =
        (static_cast<int>(bytes[10]) << 12) |
        (static_cast<int>(bytes[11]) << 4) |
        (static_cast<int>(bytes[12]) >> 4);
    result.channels = ((static_cast<int>(bytes[12]) >> 1) & 0x07) + 1;
    result.bitsPerSample =
        (((static_cast<int>(bytes[12]) & 0x01) << 4) |
         (static_cast<int>(bytes[13]) >> 4)) + 1;
    result.totalSamples =
        (static_cast<std::uint64_t>(bytes[13] & 0x0fU) << 32U) |
        static_cast<std::uint64_t>(readBigEndian32(bytes + 14));

    if (result.sampleRate <= 0 ||
        result.channels < 1 || result.channels > 8 ||
        result.bitsPerSample < 4 || result.bitsPerSample > 32) {
        result.status = FlacStreamInfoStatus::MalformedStreamInfo;
        return result;
    }
    if (codecpar->sample_rate > 0 && codecpar->sample_rate != result.sampleRate) {
        result.status = FlacStreamInfoStatus::SampleRateMismatch;
        return result;
    }
    if (codecpar->ch_layout.nb_channels > 0 &&
        codecpar->ch_layout.nb_channels != result.channels) {
        result.status = FlacStreamInfoStatus::ChannelCountMismatch;
        return result;
    }
    if (result.totalSamples == 0) {
        result.status = FlacStreamInfoStatus::TotalSamplesUnknown;
        return result;
    }

    result.status = FlacStreamInfoStatus::Valid;
    return result;
}

const char* flacStreamInfoStatusName(FlacStreamInfoStatus status) noexcept {
    switch (status) {
        case FlacStreamInfoStatus::Valid:
            return "valid";
        case FlacStreamInfoStatus::UnsupportedCodec:
            return "unsupported_codec";
        case FlacStreamInfoStatus::MissingExtradata:
            return "missing_extradata";
        case FlacStreamInfoStatus::TruncatedExtradata:
            return "truncated_extradata";
        case FlacStreamInfoStatus::UnsupportedLayout:
            return "unsupported_layout";
        case FlacStreamInfoStatus::MalformedStreamInfo:
            return "malformed_streaminfo";
        case FlacStreamInfoStatus::SampleRateMismatch:
            return "sample_rate_mismatch";
        case FlacStreamInfoStatus::ChannelCountMismatch:
            return "channel_count_mismatch";
        case FlacStreamInfoStatus::TotalSamplesUnknown:
            return "total_samples_unknown";
    }
    return "missing_extradata";
}

}  // namespace AveMediaBridge::Probe
