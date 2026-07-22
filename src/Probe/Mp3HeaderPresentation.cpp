#include "Mp3HeaderPresentation.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace AveMediaBridge::Probe {
namespace {

constexpr std::size_t kId3HeaderBytes = 10;
constexpr std::size_t kTailWindowBytes = 4096;
constexpr std::size_t kFrameSearchBytes = 64 * 1024;
constexpr std::size_t kFfmpegXingFlags = 0x0f;
constexpr std::size_t kFfmpegTagCrcBytes = 190;
constexpr std::string_view kValidatedLavcEncoderProfile = "Lavc61.19";

class BudgetExceeded final : public std::runtime_error {
public:
    BudgetExceeded() : std::runtime_error("MP3 header read budget exhausted") {}
};

class ReadFailure final : public std::runtime_error {
public:
    explicit ReadFailure(const char* message) : std::runtime_error(message) {}
};

struct ByteRange {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
};

class BoundedFileReader {
public:
    BoundedFileReader(
        const std::filesystem::path& path,
        const Mp3HeaderPresentationProbeOptions& options)
        : options_(options), stream_(path, std::ios::binary) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error || !stream_) {
            throw ReadFailure("input is not a readable regular file");
        }
        fileSize_ = std::filesystem::file_size(path, error);
        if (error) {
            throw ReadFailure("failed to determine input size");
        }
    }

    std::vector<std::uint8_t> readAt(std::uint64_t offset, std::size_t requested) {
        if (options_.forceInputNonSeekable || options_.forceSeekFailure) {
            throw ReadFailure("input seek is unavailable");
        }
        if (offset > fileSize_) {
            throw ReadFailure("read offset exceeds file size");
        }

        const std::uint64_t available = fileSize_ - offset;
        const std::uint64_t desired = (std::min)(
            static_cast<std::uint64_t>(requested), available);
        if (desired == 0) {
            return {};
        }
        const std::uint64_t remaining = options_.hardReadBudgetBytes > actualReadBytes_
            ? options_.hardReadBudgetBytes - actualReadBytes_
            : 0;
        if (desired > remaining) {
            throw BudgetExceeded();
        }
        if (actualReadBytes_ >= options_.forceReadErrorAfterBytes ||
            desired > options_.forceReadErrorAfterBytes - actualReadBytes_) {
            throw ReadFailure("forced bounded reader failure");
        }

        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        ++seekCallCount_;
        if (!stream_) {
            throw ReadFailure("input seek failed");
        }

        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(desired));
        stream_.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        ++readCallCount_;
        const std::streamsize read = stream_.gcount();
        if (read < 0) {
            throw ReadFailure("input read failed");
        }
        bytes.resize(static_cast<std::size_t>(read));
        actualReadBytes_ += bytes.size();
        maximumOffsetReached_ = (std::max)(
            maximumOffsetReached_, offset + static_cast<std::uint64_t>(bytes.size()));
        if (!bytes.empty()) {
            ranges_.push_back(ByteRange{
                offset, offset + static_cast<std::uint64_t>(bytes.size())});
        }
        if (bytes.size() != desired) {
            throw ReadFailure("short input read");
        }
        return bytes;
    }

    std::uint64_t fileSize() const noexcept { return fileSize_; }
    std::uint64_t actualReadBytes() const noexcept { return actualReadBytes_; }
    std::uint64_t readCallCount() const noexcept { return readCallCount_; }
    std::uint64_t seekCallCount() const noexcept { return seekCallCount_; }
    std::uint64_t maximumOffsetReached() const noexcept { return maximumOffsetReached_; }

    std::uint64_t uniqueBytesRead() const {
        std::vector<ByteRange> ranges = ranges_;
        std::sort(ranges.begin(), ranges.end(), [](const ByteRange& left, const ByteRange& right) {
            return left.begin < right.begin ||
                (left.begin == right.begin && left.end < right.end);
        });
        std::uint64_t total = 0;
        std::uint64_t coveredEnd = 0;
        bool hasCoveredRange = false;
        for (const ByteRange& range : ranges) {
            if (!hasCoveredRange || range.begin > coveredEnd) {
                total += range.end - range.begin;
                coveredEnd = range.end;
                hasCoveredRange = true;
            } else if (range.end > coveredEnd) {
                total += range.end - coveredEnd;
                coveredEnd = range.end;
            }
        }
        return total;
    }

private:
    Mp3HeaderPresentationProbeOptions options_;
    std::ifstream stream_;
    std::uint64_t fileSize_ = 0;
    std::uint64_t actualReadBytes_ = 0;
    std::uint64_t readCallCount_ = 0;
    std::uint64_t seekCallCount_ = 0;
    std::uint64_t maximumOffsetReached_ = 0;
    std::vector<ByteRange> ranges_;
};

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

struct LeadingId3 {
    bool valid = true;
    std::uint64_t totalBytes = 0;
};

enum class TailStatus {
    Valid,
    Unsupported,
    Invalid
};

struct TrailingTags {
    TailStatus status = TailStatus::Valid;
    std::uint64_t audioEnd = 0;
};

struct XingInfo {
    bool present = false;
    bool truncated = false;
    Mp3HeaderType type = Mp3HeaderType::None;
    std::uint32_t flags = 0;
    std::uint64_t frameCount = 0;
    std::uint64_t byteCount = 0;
    std::size_t offset = 0;
    std::size_t extensionOffset = 0;
};

struct VbriInfo {
    bool present = false;
    std::uint16_t version = 0;
    std::uint64_t frameCount = 0;
    std::uint64_t byteCount = 0;
};

struct GaplessInfo {
    bool present = false;
    bool validated = false;
    std::string encoder;
    std::uint64_t delay = 0;
    std::uint64_t padding = 0;
    std::uint64_t musicLength = 0;
};

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

std::uint32_t readLittleEndian32(const std::uint8_t* bytes) noexcept {
    return
        static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

bool checkedMultiply(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& product) noexcept {
    if (left != 0 && right > (std::numeric_limits<std::uint64_t>::max)() / left) {
        return false;
    }
    product = left * right;
    return true;
}

bool checkedAdd(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& sum) noexcept {
    if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
        return false;
    }
    sum = left + right;
    return true;
}

bool syncSafe32(const std::uint8_t* bytes, std::uint32_t& value) noexcept {
    if ((bytes[0] | bytes[1] | bytes[2] | bytes[3]) & 0x80U) {
        return false;
    }
    value =
        (static_cast<std::uint32_t>(bytes[0]) << 21U) |
        (static_cast<std::uint32_t>(bytes[1]) << 14U) |
        (static_cast<std::uint32_t>(bytes[2]) << 7U) |
        static_cast<std::uint32_t>(bytes[3]);
    return true;
}

MpegFrameHeader parseMpegFrameHeader(const std::uint8_t* bytes, std::size_t size) noexcept {
    MpegFrameHeader result;
    if (!bytes || size < 4) {
        return result;
    }
    const std::uint32_t value = readBigEndian32(bytes);
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

bool sameMpegDomain(const MpegFrameHeader& left, const MpegFrameHeader& right) noexcept {
    return left.valid && right.valid &&
        left.version == right.version &&
        left.layer == right.layer &&
        left.sampleRate == right.sampleRate;
}

LeadingId3 parseLeadingId3(BoundedFileReader& reader) {
    LeadingId3 result;
    const std::vector<std::uint8_t> header = reader.readAt(0, kId3HeaderBytes);
    if (header.size() < kId3HeaderBytes ||
        header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
        return result;
    }
    const int major = header[3];
    const std::uint8_t flags = header[5];
    const std::uint8_t legalFlags = major == 2 ? 0xc0U : major == 3 ? 0xe0U : major == 4 ? 0xf0U : 0;
    std::uint32_t payloadBytes = 0;
    if (legalFlags == 0 || (flags & ~legalFlags) != 0 ||
        !syncSafe32(header.data() + 6, payloadBytes)) {
        result.valid = false;
        return result;
    }
    const bool footer = major == 4 && (flags & 0x10U) != 0;
    std::uint64_t total = kId3HeaderBytes;
    if (!checkedAdd(total, payloadBytes, total) ||
        (footer && !checkedAdd(total, kId3HeaderBytes, total)) ||
        total > reader.fileSize()) {
        result.valid = false;
        return result;
    }
    if (footer) {
        const std::vector<std::uint8_t> footerBytes = reader.readAt(10U + payloadBytes, 10);
        std::uint32_t footerPayload = 0;
        if (footerBytes.size() != 10 ||
            footerBytes[0] != '3' || footerBytes[1] != 'D' || footerBytes[2] != 'I' ||
            footerBytes[3] != header[3] || footerBytes[4] != header[4] ||
            footerBytes[5] != header[5] ||
            !syncSafe32(footerBytes.data() + 6, footerPayload) ||
            footerPayload != payloadBytes) {
            result.valid = false;
            return result;
        }
    }
    result.totalBytes = total;
    return result;
}

bool allZero(const std::uint8_t* bytes, std::size_t size) noexcept {
    for (std::size_t index = 0; index < size; ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    return true;
}

TrailingTags parseTrailingTags(BoundedFileReader& reader) {
    TrailingTags result;
    result.audioEnd = reader.fileSize();
    const std::uint64_t tailOffset = result.audioEnd > kTailWindowBytes
        ? result.audioEnd - kTailWindowBytes
        : 0;
    const std::vector<std::uint8_t> tail = reader.readAt(
        tailOffset,
        static_cast<std::size_t>(result.audioEnd - tailOffset));
    auto readBytes = [&](std::uint64_t offset, std::size_t size) {
        if (offset >= tailOffset && offset + size <= reader.fileSize()) {
            const std::size_t relative = static_cast<std::size_t>(offset - tailOffset);
            if (relative + size <= tail.size()) {
                return std::vector<std::uint8_t>(
                    tail.begin() + relative, tail.begin() + relative + size);
            }
        }
        return reader.readAt(offset, size);
    };

    if (result.audioEnd >= 128) {
        const std::vector<std::uint8_t> signature = readBytes(result.audioEnd - 128, 3);
        if (signature.size() == 3 && signature[0] == 'T' && signature[1] == 'A' && signature[2] == 'G') {
            result.audioEnd -= 128;
        }
    }
    if (result.audioEnd >= 32) {
        const std::vector<std::uint8_t> footer = readBytes(result.audioEnd - 32, 32);
        if (footer.size() == 32 &&
            std::equal(footer.begin(), footer.begin() + 8, "APETAGEX")) {
            const std::uint32_t version = readLittleEndian32(footer.data() + 8);
            const std::uint32_t size = readLittleEndian32(footer.data() + 12);
            const std::uint32_t itemCount = readLittleEndian32(footer.data() + 16);
            const std::uint32_t flags = readLittleEndian32(footer.data() + 20);
            if ((version != 1000 && version != 2000) || size < 32 ||
                size > result.audioEnd || itemCount > 1000000 ||
                flags != 0 || !allZero(footer.data() + 24, 8)) {
                result.status = TailStatus::Unsupported;
                return result;
            }
            result.audioEnd -= size;
        }
    }
    return result;
}

std::size_t sideInfoBytes(const MpegFrameHeader& header) noexcept {
    if (header.version == 1) {
        return header.channels == 1 ? 17 : 32;
    }
    return header.channels == 1 ? 9 : 17;
}

XingInfo parseXingInfo(
    const std::vector<std::uint8_t>& frame,
    const MpegFrameHeader& header) noexcept {
    XingInfo result;
    const std::size_t offset = 4 + (header.crcPresent ? 2 : 0) + sideInfoBytes(header);
    if (offset + 8 > frame.size()) {
        return result;
    }
    const bool xing = std::equal(frame.begin() + offset, frame.begin() + offset + 4, "Xing");
    const bool info = std::equal(frame.begin() + offset, frame.begin() + offset + 4, "Info");
    if (!xing && !info) {
        return result;
    }
    result.present = true;
    result.type = info ? Mp3HeaderType::Info : Mp3HeaderType::Xing;
    result.offset = offset;
    result.flags = readBigEndian32(frame.data() + offset + 4);
    std::size_t cursor = offset + 8;
    auto take = [&](std::size_t size, const std::uint8_t*& bytes) {
        if (cursor + size > frame.size()) {
            result.truncated = true;
            bytes = nullptr;
            return false;
        }
        bytes = frame.data() + cursor;
        cursor += size;
        return true;
    };
    const std::uint8_t* bytes = nullptr;
    if ((result.flags & 1U) != 0 && take(4, bytes)) {
        result.frameCount = readBigEndian32(bytes);
    }
    if ((result.flags & 2U) != 0 && take(4, bytes)) {
        result.byteCount = readBigEndian32(bytes);
    }
    if ((result.flags & 4U) != 0) {
        take(100, bytes);
    }
    if ((result.flags & 8U) != 0) {
        take(4, bytes);
    }
    result.extensionOffset = cursor;
    return result;
}

VbriInfo parseVbri(const std::vector<std::uint8_t>& frame) noexcept {
    VbriInfo result;
    constexpr std::size_t offset = 36;
    if (offset + 26 > frame.size() ||
        !std::equal(frame.begin() + offset, frame.begin() + offset + 4, "VBRI")) {
        return result;
    }
    result.present = true;
    result.version = readBigEndian16(frame.data() + offset + 4);
    result.byteCount = readBigEndian32(frame.data() + offset + 10);
    result.frameCount = readBigEndian32(frame.data() + offset + 14);
    return result;
}

std::uint16_t crc16AnsiLittleEndian(std::vector<std::uint8_t> bytes) noexcept {
    std::uint16_t crc = 0;
    for (std::uint8_t value : bytes) {
        crc ^= value;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0
                ? static_cast<std::uint16_t>((crc >> 1U) ^ 0xa001U)
                : static_cast<std::uint16_t>(crc >> 1U);
        }
    }
    return crc;
}

GaplessInfo parseFfmpegLavcGapless(
    const std::vector<std::uint8_t>& frame,
    const XingInfo& xing) {
    GaplessInfo result;
    if (!xing.present || xing.truncated || xing.flags != kFfmpegXingFlags ||
        xing.extensionOffset != xing.offset + 120 ||
        xing.extensionOffset + 36 > frame.size() ||
        frame.size() < kFfmpegTagCrcBytes) {
        return result;
    }
    const std::uint8_t* extension = frame.data() + xing.extensionOffset;
    result.encoder.assign(reinterpret_cast<const char*>(extension), 9);
    while (!result.encoder.empty() &&
           (result.encoder.back() == '\0' || result.encoder.back() == ' ')) {
        result.encoder.pop_back();
    }
    result.present = !result.encoder.empty();
    if (result.encoder != kValidatedLavcEncoderProfile || extension[9] != 0) {
        return result;
    }

    const std::uint32_t delayPadding =
        (static_cast<std::uint32_t>(extension[21]) << 16U) |
        (static_cast<std::uint32_t>(extension[22]) << 8U) |
        static_cast<std::uint32_t>(extension[23]);
    result.delay = (delayPadding >> 12U) & 0x0fffU;
    result.padding = delayPadding & 0x0fffU;
    result.musicLength = readBigEndian32(extension + 28);
    const std::uint16_t storedCrc = readBigEndian16(extension + 34);
    const std::size_t crcOffset = xing.extensionOffset + 34;
    std::vector<std::uint8_t> crcInput(frame.begin(), frame.begin() + kFfmpegTagCrcBytes);
    if (crcOffset < crcInput.size()) {
        crcInput[crcOffset] = 0;
        if (crcOffset + 1 < crcInput.size()) {
            crcInput[crcOffset + 1] = 0;
        }
    }
    result.validated = storedCrc == crc16AnsiLittleEndian(std::move(crcInput));
    return result;
}

std::uint64_t frameSizeFor(
    const MpegFrameHeader& header,
    int bitrateKbps,
    int padding) noexcept {
    const std::uint64_t coefficient = header.version == 1 ? 144 : 72;
    return coefficient * static_cast<std::uint64_t>(bitrateKbps) * 1000U /
        static_cast<std::uint64_t>(header.sampleRate) +
        static_cast<std::uint64_t>(padding);
}

bool byteCountPlausible(
    const MpegFrameHeader& metadataHeader,
    const MpegFrameHeader& firstAudioHeader,
    const XingInfo& xing,
    std::uint64_t audioDataBytes) noexcept {
    if (!xing.present || xing.frameCount == 0 || xing.byteCount != audioDataBytes ||
        !sameMpegDomain(metadataHeader, firstAudioHeader)) {
        return false;
    }

    std::uint64_t minimumFrameBytes = 0;
    std::uint64_t maximumFrameBytes = 0;
    if (xing.type == Mp3HeaderType::Info) {
        minimumFrameBytes = frameSizeFor(firstAudioHeader, firstAudioHeader.bitrateKbps, 0);
        maximumFrameBytes = frameSizeFor(firstAudioHeader, firstAudioHeader.bitrateKbps, 1);
    } else {
        static constexpr std::array<int, 14> kMpeg1Layer3Bitrates{
            32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
        static constexpr std::array<int, 14> kMpeg2Layer3Bitrates{
            8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
        const auto& bitrates = metadataHeader.version == 1
            ? kMpeg1Layer3Bitrates
            : kMpeg2Layer3Bitrates;
        minimumFrameBytes = (std::numeric_limits<std::uint64_t>::max)();
        for (int bitrate : bitrates) {
            minimumFrameBytes = (std::min)(
                minimumFrameBytes, frameSizeFor(metadataHeader, bitrate, 0));
            maximumFrameBytes = (std::max)(
                maximumFrameBytes, frameSizeFor(metadataHeader, bitrate, 1));
        }
    }

    std::uint64_t minimumAudioBytes = 0;
    std::uint64_t maximumAudioBytes = 0;
    std::uint64_t minimumTotal = 0;
    std::uint64_t maximumTotal = 0;
    return checkedMultiply(xing.frameCount, minimumFrameBytes, minimumAudioBytes) &&
        checkedMultiply(xing.frameCount, maximumFrameBytes, maximumAudioBytes) &&
        checkedAdd(metadataHeader.frameSizeBytes, minimumAudioBytes, minimumTotal) &&
        checkedAdd(metadataHeader.frameSizeBytes, maximumAudioBytes, maximumTotal) &&
        audioDataBytes >= minimumTotal && audioDataBytes <= maximumTotal;
}

void finishIoMetrics(
    Mp3HeaderPresentationResult& result,
    const BoundedFileReader& reader) {
    result.fileSizeBytes = reader.fileSize();
    result.totalActualReadBytes = reader.actualReadBytes();
    result.uniqueBytesRead = reader.uniqueBytesRead();
    result.duplicateBytesRead = result.totalActualReadBytes - result.uniqueBytesRead;
    result.readCallCount = reader.readCallCount();
    result.seekCallCount = reader.seekCallCount();
    result.maximumOffsetReached = reader.maximumOffsetReached();
    result.maximumBudgetOverrunBytes = result.totalActualReadBytes > result.hardReadBudgetBytes
        ? result.totalActualReadBytes - result.hardReadBudgetBytes
        : 0;
}

bool formatListContains(const char* formatNames, std::string_view expected) noexcept {
    if (!formatNames) {
        return false;
    }
    std::string_view remaining(formatNames);
    while (!remaining.empty()) {
        const std::size_t comma = remaining.find(',');
        if (remaining.substr(0, comma) == expected) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(comma + 1);
    }
    return false;
}

}  // namespace

bool shouldProbeMp3HeaderPresentation(
    const AVFormatContext* formatContext,
    const AVStream* audioStream,
    bool strongerExactAuthority) noexcept {
    return !strongerExactAuthority && formatContext && formatContext->iformat &&
        audioStream && audioStream->codecpar &&
        formatListContains(formatContext->iformat->name, "mp3") &&
        audioStream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
        audioStream->codecpar->codec_id == AV_CODEC_ID_MP3 &&
        audioStream->codecpar->sample_rate > 0 &&
        audioStream->codecpar->ch_layout.nb_channels > 0;
}

bool mp3HeaderPresentationMatchesStream(
    const Mp3HeaderPresentationResult& result,
    const AVStream* audioStream) noexcept {
    return result.exact() && audioStream && audioStream->codecpar &&
        audioStream->codecpar->codec_id == AV_CODEC_ID_MP3 &&
        result.sampleRate == audioStream->codecpar->sample_rate &&
        result.channels == audioStream->codecpar->ch_layout.nb_channels;
}

Mp3HeaderPresentationResult probeMp3HeaderPresentation(
    const std::string& path,
    Mp3HeaderPresentationProbeOptions options) {
    Mp3HeaderPresentationResult result;
    result.hardReadBudgetBytes = options.hardReadBudgetBytes;
    if (path.empty() || options.forceInputNonSeekable || options.hardReadBudgetBytes == 0) {
        result.reason = options.hardReadBudgetBytes == 0
            ? "read_budget_unavailable"
            : "seekable_local_input_required";
        return result;
    }

    try {
        BoundedFileReader reader(std::filesystem::u8path(path), options);
        result.fileSizeBytes = reader.fileSize();
        try {
            const LeadingId3 id3 = parseLeadingId3(reader);
            result.validation.leadingTagValidated = id3.valid;
            if (!id3.valid) {
                result.status = Mp3HeaderPresentationStatus::InvalidMedia;
                result.reason = "invalid_id3v2_structure";
                finishIoMetrics(result, reader);
                return result;
            }

            const TrailingTags trailing = parseTrailingTags(reader);
            result.validation.trailingTagsValidated = trailing.status == TailStatus::Valid;
            if (trailing.status != TailStatus::Valid) {
                result.status = trailing.status == TailStatus::Invalid
                    ? Mp3HeaderPresentationStatus::InvalidMedia
                    : Mp3HeaderPresentationStatus::Unavailable;
                result.reason = "unsupported_or_invalid_trailing_tag_layout";
                finishIoMetrics(result, reader);
                return result;
            }
            if (id3.totalBytes >= trailing.audioEnd) {
                result.status = Mp3HeaderPresentationStatus::InvalidMedia;
                result.reason = "audio_range_is_empty";
                finishIoMetrics(result, reader);
                return result;
            }

            const std::size_t searchBytes = static_cast<std::size_t>((std::min)(
                static_cast<std::uint64_t>(kFrameSearchBytes),
                trailing.audioEnd - id3.totalBytes));
            const std::vector<std::uint8_t> head = reader.readAt(id3.totalBytes, searchBytes);
            const MpegFrameHeader first = parseMpegFrameHeader(head.data(), head.size());
            if (!first.valid) {
                result.status = Mp3HeaderPresentationStatus::InvalidMedia;
                result.reason = "valid_mpeg_layer3_frame_not_found_at_audio_start";
                finishIoMetrics(result, reader);
                return result;
            }
            result.validation.firstFrameValidated = true;
            result.mpegVersion = first.version;
            result.layer = first.layer;
            result.sampleRate = first.sampleRate;
            result.channels = first.channels;
            result.samplesPerFrame = first.samplesPerFrame;
            if (first.frameSizeBytes > trailing.audioEnd - id3.totalBytes) {
                result.status = Mp3HeaderPresentationStatus::InvalidMedia;
                result.reason = "truncated_first_mpeg_frame";
                finishIoMetrics(result, reader);
                return result;
            }

            const std::vector<std::uint8_t> frame = reader.readAt(
                id3.totalBytes, static_cast<std::size_t>(first.frameSizeBytes));
            const std::uint64_t secondOffset = id3.totalBytes + first.frameSizeBytes;
            const std::vector<std::uint8_t> secondBytes = reader.readAt(secondOffset, 4);
            const MpegFrameHeader second = parseMpegFrameHeader(
                secondBytes.data(), secondBytes.size());
            result.validation.secondFrameValidated = sameMpegDomain(first, second);

            const XingInfo xing = parseXingInfo(frame, first);
            const VbriInfo vbri = parseVbri(frame);
            if (xing.present && vbri.present) {
                result.status = Mp3HeaderPresentationStatus::Conflict;
                result.reason = "mutually_exclusive_xing_and_vbri_signatures_present";
                finishIoMetrics(result, reader);
                return result;
            }
            result.audioDataBytes = trailing.audioEnd - id3.totalBytes;
            if (!xing.present) {
                result.headerType = vbri.present ? Mp3HeaderType::Vbri : Mp3HeaderType::None;
                result.physicalFrameCount = vbri.frameCount;
                result.declaredAudioBytes = vbri.byteCount;
                if (vbri.present && vbri.version == 1 && vbri.frameCount > 0 &&
                    vbri.byteCount == result.audioDataBytes &&
                    result.validation.secondFrameValidated &&
                    checkedMultiply(
                        vbri.frameCount, first.samplesPerFrame,
                        result.physicalSampleTotal)) {
                    result.validation.byteRangeValidated = true;
                    result.validation.checkedArithmetic = true;
                    result.reason =
                        "vbri_physical_total_has_no_validated_gapless_trim";
                } else {
                    result.reason = vbri.present
                        ? "vbri_physical_total_not_validated"
                        : "validated_info_or_xing_header_unavailable";
                }
                finishIoMetrics(result, reader);
                return result;
            }

            result.headerType = xing.type;
            result.physicalFrameCount = xing.frameCount;
            result.declaredAudioBytes = xing.byteCount;
            if (xing.truncated || xing.flags != kFfmpegXingFlags ||
                xing.frameCount == 0 || xing.byteCount == 0 ||
                !result.validation.secondFrameValidated ||
                !byteCountPlausible(first, second, xing, result.audioDataBytes)) {
                result.reason = "info_xing_frame_or_byte_total_not_validated";
                finishIoMetrics(result, reader);
                return result;
            }
            result.validation.byteRangeValidated = true;

            const GaplessInfo gapless = parseFfmpegLavcGapless(frame, xing);
            result.encoderProfile = gapless.encoder;
            result.validation.encoderProfileValidated =
                gapless.present && gapless.encoder == kValidatedLavcEncoderProfile;
            result.validation.tagCrcValidated = gapless.validated;
            if (!result.validation.encoderProfileValidated || !gapless.validated) {
                result.reason = "ffmpeg_lavc_gapless_profile_not_validated";
                finishIoMetrics(result, reader);
                return result;
            }
            if (gapless.musicLength != result.audioDataBytes) {
                result.status = Mp3HeaderPresentationStatus::Conflict;
                result.reason = "gapless_music_length_conflicts_with_audio_range";
                finishIoMetrics(result, reader);
                return result;
            }

            if (!checkedMultiply(
                    xing.frameCount, first.samplesPerFrame, result.physicalSampleTotal)) {
                result.status = Mp3HeaderPresentationStatus::InvalidMedia;
                result.reason = "physical_sample_total_overflow";
                finishIoMetrics(result, reader);
                return result;
            }
            result.validation.checkedArithmetic = true;
            std::uint64_t trim = 0;
            if (!checkedAdd(gapless.delay, gapless.padding, trim) ||
                trim > result.physicalSampleTotal) {
                result.status = Mp3HeaderPresentationStatus::InvalidMedia;
                result.reason = "gapless_trim_exceeds_physical_total";
                finishIoMetrics(result, reader);
                return result;
            }
            result.initialPresentationSkip = gapless.delay;
            result.terminalPresentationPadding = gapless.padding;
            result.presentationFrames = result.physicalSampleTotal - trim;
            result.validation.gaplessFieldsValidated = true;
            if (result.presentationFrames == 0) {
                result.status = Mp3HeaderPresentationStatus::InvalidMedia;
                result.reason = "empty_mp3_presentation";
                finishIoMetrics(result, reader);
                return result;
            }

            result.status = Mp3HeaderPresentationStatus::Exact;
            result.trust = PresentationTotalTrust::SampleExact;
            result.source = PresentationTotalSource::Mp3ValidatedHeaderPresentation;
            result.domain = PresentationSampleDomain::NativeStreamSamples;
            result.reason = "validated_ffmpeg_lavc_info_xing_gapless_presentation";
            finishIoMetrics(result, reader);
            return result;
        } catch (const BudgetExceeded&) {
            result.status = Mp3HeaderPresentationStatus::IoBudgetExceeded;
            result.reason = "header_read_budget_exhausted";
            finishIoMetrics(result, reader);
            return result;
        } catch (const ReadFailure&) {
            result.status = Mp3HeaderPresentationStatus::Unavailable;
            result.reason = "bounded_header_read_failed";
            finishIoMetrics(result, reader);
            return result;
        }
    } catch (const ReadFailure&) {
        result.status = Mp3HeaderPresentationStatus::Unavailable;
        result.reason = "seekable_local_input_required";
        return result;
    } catch (const std::filesystem::filesystem_error&) {
        result.status = Mp3HeaderPresentationStatus::Unavailable;
        result.reason = "input_file_unavailable";
        return result;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        result.status = Mp3HeaderPresentationStatus::InvalidMedia;
        result.reason = "unexpected_header_parser_failure";
        return result;
    }
}

const char* mp3HeaderPresentationStatusName(
    Mp3HeaderPresentationStatus status) noexcept {
    switch (status) {
        case Mp3HeaderPresentationStatus::Exact:
            return "exact";
        case Mp3HeaderPresentationStatus::Unavailable:
            return "unavailable";
        case Mp3HeaderPresentationStatus::IoBudgetExceeded:
            return "io_budget_exceeded";
        case Mp3HeaderPresentationStatus::Conflict:
            return "conflict";
        case Mp3HeaderPresentationStatus::InvalidMedia:
            return "invalid_media";
    }
    return "unavailable";
}

const char* mp3HeaderTypeName(Mp3HeaderType type) noexcept {
    switch (type) {
        case Mp3HeaderType::None:
            return "none";
        case Mp3HeaderType::Info:
            return "info";
        case Mp3HeaderType::Xing:
            return "xing";
        case Mp3HeaderType::Vbri:
            return "vbri";
    }
    return "none";
}

}  // namespace AveMediaBridge::Probe
