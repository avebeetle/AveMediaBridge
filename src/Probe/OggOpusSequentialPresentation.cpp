#include "OggOpusSequentialPresentation.hpp"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace AveMediaBridge::Probe {
namespace {

constexpr std::size_t kMaximumOggPageBodyBytes = 255U * 255U;
constexpr std::size_t kMaximumOpusHeadBytes = 21U + 255U;
constexpr std::size_t kMaximumOpusFrameBytes = 1275U;
constexpr int kMaximumOpusFramesPerPacket = 48;
constexpr int kCanonicalOpusSampleRate = 48000;
constexpr std::int64_t kUnknownGranule = -1;
constexpr std::uint32_t kMaximumLogicalStreams = 1000;

OggOpusSequentialPresentationResult finishTimedScan(
    OggOpusSequentialPresentationResult result,
    std::chrono::steady_clock::time_point started) noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    result.scanDurationUs = elapsed > 0
        ? static_cast<std::uint64_t>(elapsed)
        : 0;
    return result;
}

bool formatListContains(const char* names, std::string_view expected) noexcept {
    if (!names || expected.empty()) {
        return false;
    }
    std::string_view remaining(names);
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

std::uint16_t little16(const std::uint8_t* data) noexcept {
    return static_cast<std::uint16_t>(data[0]) |
        (static_cast<std::uint16_t>(data[1]) << 8);
}

std::uint32_t little32(const std::uint8_t* data) noexcept {
    return static_cast<std::uint32_t>(data[0]) |
        (static_cast<std::uint32_t>(data[1]) << 8) |
        (static_cast<std::uint32_t>(data[2]) << 16) |
        (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint64_t little64(const std::uint8_t* data) noexcept {
    std::uint64_t value = 0;
    for (int index = 7; index >= 0; --index) {
        value = (value << 8) | data[index];
    }
    return value;
}

bool checkedAdd(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& result) noexcept {
    if (left > (std::numeric_limits<std::uint64_t>::max)() - right) {
        return false;
    }
    result = left + right;
    return true;
}

bool checkedMultiply(
    std::size_t left,
    std::size_t right,
    std::size_t& result) noexcept {
    if (left != 0 && right > (std::numeric_limits<std::size_t>::max)() / left) {
        return false;
    }
    result = left * right;
    return true;
}

struct OpusHead {
    bool valid = false;
    OggOpusSequentialReason reason = OggOpusSequentialReason::InvalidOpusHead;
    int version = 0;
    int channels = 0;
    std::uint16_t preSkip = 0;
    std::uint32_t inputSampleRate = 0;
    std::int16_t outputGain = 0;
    std::uint8_t mappingFamily = 0;
    std::uint8_t streamCount = 0;
    std::uint8_t coupledCount = 0;
};

OpusHead parseOpusHead(const std::uint8_t* packet, std::size_t size) noexcept {
    OpusHead result;
    if (!packet || size < 19 || std::memcmp(packet, "OpusHead", 8) != 0) {
        return result;
    }
    result.version = packet[8];
    result.channels = packet[9];
    result.preSkip = little16(packet + 10);
    result.inputSampleRate = little32(packet + 12);
    result.outputGain = static_cast<std::int16_t>(little16(packet + 16));
    result.mappingFamily = packet[18];
    if (result.version > 15) {
        result.reason = OggOpusSequentialReason::UnsupportedOpusHeadVersion;
        return result;
    }
    if (result.channels <= 0) {
        return result;
    }
    if (result.mappingFamily == 0) {
        if ((result.channels != 1 && result.channels != 2) || size != 19) {
            return result;
        }
        result.streamCount = 1;
        result.coupledCount = result.channels == 2 ? 1 : 0;
    } else {
        const std::size_t required = 21U + static_cast<std::size_t>(result.channels);
        if (size != required) {
            return result;
        }
        result.streamCount = packet[19];
        result.coupledCount = packet[20];
        if (result.streamCount == 0 || result.coupledCount > result.streamCount) {
            return result;
        }
        const int codedChannels = result.streamCount + result.coupledCount;
        for (std::size_t index = 21; index < required; ++index) {
            if (packet[index] != 255 && packet[index] >= codedChannels) {
                return result;
            }
        }
    }
    result.valid = true;
    result.reason = OggOpusSequentialReason::CompleteContinuityProof;
    return result;
}

bool matchesIdentity(
    const OpusHead& head,
    const std::vector<std::uint8_t>& rawHead,
    const OggOpusSelectedStreamIdentity& expected) noexcept {
    return head.valid &&
        head.channels == expected.channels &&
        head.preSkip == expected.preSkip &&
        head.inputSampleRate == expected.inputSampleRate &&
        head.outputGain == expected.outputGain &&
        head.mappingFamily == expected.mappingFamily &&
        head.streamCount == expected.streamCount &&
        head.coupledCount == expected.coupledCount &&
        rawHead == expected.opusHead;
}

bool deriveMaximumSelectedPacketBytes(
    int streamCount,
    std::size_t& maximum) noexcept {
    if (streamCount <= 0 || streamCount > 255) {
        return false;
    }
    // One TOC byte, code-3 control, two-byte size declarations for every
    // possible frame, one self-delimited size, and the maximum coded bytes.
    constexpr std::size_t perStream =
        2U +
        static_cast<std::size_t>(kMaximumOpusFramesPerPacket) * 2U +
        2U +
        static_cast<std::size_t>(kMaximumOpusFramesPerPacket) *
            kMaximumOpusFrameBytes;
    return checkedMultiply(perStream, static_cast<std::size_t>(streamCount), maximum);
}

bool parseFrameSize(
    const std::uint8_t* packet,
    std::size_t limit,
    std::size_t& cursor,
    std::size_t& frameSize) noexcept {
    if (cursor >= limit) {
        return false;
    }
    const std::uint8_t first = packet[cursor++];
    if (first < 252) {
        frameSize = first;
        return true;
    }
    if (cursor >= limit) {
        return false;
    }
    frameSize = static_cast<std::size_t>(first) +
        4U * static_cast<std::size_t>(packet[cursor++]);
    return true;
}

struct ParsedSubpacket {
    bool valid = false;
    int frameCount = 0;
    int samples48k = 0;
    std::size_t packetBytes = 0;
};

ParsedSubpacket parseOpusSubpacket(
    const std::uint8_t* packet,
    std::size_t size,
    bool selfDelimited) noexcept {
    ParsedSubpacket result;
    if (!packet || size == 0) {
        return result;
    }

    const std::uint8_t toc = packet[0];
    const int configuration = toc >> 3;
    const int code = toc & 0x03;
    int samplesPerFrame = 0;
    if (configuration < 12) {
        samplesPerFrame = configuration % 4 == 0
            ? 480
            : 960 * (configuration % 4);
    } else if (configuration < 16) {
        samplesPerFrame = 480 << (configuration & 1);
    } else {
        samplesPerFrame = 120 << (configuration & 3);
    }

    int frameCount = 1;
    std::size_t cursor = 1;
    std::size_t packetEnd = size;
    if (code == 0) {
        std::size_t frameSize = size - cursor;
        if (selfDelimited) {
            if (!parseFrameSize(packet, size, cursor, frameSize) ||
                frameSize > size - cursor) {
                return result;
            }
            packetEnd = cursor + frameSize;
        }
        if (frameSize > kMaximumOpusFrameBytes) {
            return result;
        }
    } else if (code == 1) {
        frameCount = 2;
        std::size_t frameSize = 0;
        if (selfDelimited) {
            if (!parseFrameSize(packet, size, cursor, frameSize) ||
                frameSize > kMaximumOpusFrameBytes ||
                frameSize > (size - cursor) / 2U) {
                return result;
            }
            packetEnd = cursor + 2U * frameSize;
        } else {
            const std::size_t payload = size - cursor;
            if (payload % 2U != 0 || payload / 2U > kMaximumOpusFrameBytes) {
                return result;
            }
        }
    } else if (code == 2) {
        frameCount = 2;
        std::size_t firstFrameSize = 0;
        if (!parseFrameSize(packet, size, cursor, firstFrameSize) ||
            firstFrameSize > kMaximumOpusFrameBytes) {
            return result;
        }
        std::size_t secondFrameSize = 0;
        if (selfDelimited) {
            if (!parseFrameSize(packet, size, cursor, secondFrameSize) ||
                firstFrameSize > size - cursor ||
                secondFrameSize > size - cursor - firstFrameSize) {
                return result;
            }
            packetEnd = cursor + firstFrameSize + secondFrameSize;
        } else {
            if (firstFrameSize > size - cursor) {
                return result;
            }
            secondFrameSize = size - cursor - firstFrameSize;
        }
        if (secondFrameSize > kMaximumOpusFrameBytes) {
            return result;
        }
    } else {
        if (size < 2) {
            return result;
        }
        const std::uint8_t control = packet[cursor++];
        frameCount = control & 0x3F;
        if (frameCount <= 0 || frameCount > kMaximumOpusFramesPerPacket) {
            return result;
        }

        std::size_t padding = 0;
        if ((control & 0x40) != 0) {
            std::uint8_t value = 255;
            while (value == 255) {
                if (cursor >= size) {
                    return result;
                }
                value = packet[cursor++];
                const std::size_t add = value == 255 ? 254U : value;
                if (padding > (std::numeric_limits<std::size_t>::max)() - add) {
                    return result;
                }
                padding += add;
            }
        }

        const bool vbr = (control & 0x80) != 0;
        std::size_t declaredFrameBytes = 0;
        if (vbr) {
            for (int index = 0; index < frameCount - 1; ++index) {
                std::size_t frameSize = 0;
                if (!parseFrameSize(packet, size, cursor, frameSize) ||
                    frameSize > kMaximumOpusFrameBytes ||
                    declaredFrameBytes >
                        (std::numeric_limits<std::size_t>::max)() - frameSize) {
                    return result;
                }
                declaredFrameBytes += frameSize;
            }
            if (selfDelimited) {
                std::size_t lastFrameSize = 0;
                if (!parseFrameSize(packet, size, cursor, lastFrameSize) ||
                    lastFrameSize > kMaximumOpusFrameBytes ||
                    declaredFrameBytes >
                        (std::numeric_limits<std::size_t>::max)() - lastFrameSize) {
                    return result;
                }
                declaredFrameBytes += lastFrameSize;
                if (declaredFrameBytes > size - cursor ||
                    padding > size - cursor - declaredFrameBytes) {
                    return result;
                }
                packetEnd = cursor + declaredFrameBytes + padding;
            } else {
                if (padding > size - cursor ||
                    declaredFrameBytes > size - cursor - padding) {
                    return result;
                }
                const std::size_t lastFrameSize =
                    size - cursor - padding - declaredFrameBytes;
                if (lastFrameSize > kMaximumOpusFrameBytes) {
                    return result;
                }
            }
        } else if (selfDelimited) {
            std::size_t frameSize = 0;
            if (!parseFrameSize(packet, size, cursor, frameSize) ||
                frameSize > kMaximumOpusFrameBytes) {
                return result;
            }
            std::size_t payloadBytes = 0;
            if (!checkedMultiply(
                    frameSize, static_cast<std::size_t>(frameCount), payloadBytes) ||
                payloadBytes > size - cursor ||
                padding > size - cursor - payloadBytes) {
                return result;
            }
            packetEnd = cursor + payloadBytes + padding;
        } else {
            if (padding > size - cursor) {
                return result;
            }
            const std::size_t payload = size - cursor - padding;
            if (payload % static_cast<std::size_t>(frameCount) != 0 ||
                payload / static_cast<std::size_t>(frameCount) >
                    kMaximumOpusFrameBytes) {
                return result;
            }
        }
    }

    if (samplesPerFrame <= 0 ||
        samplesPerFrame > 5760 / frameCount ||
        packetEnd == 0 || packetEnd > size) {
        return result;
    }
    result.valid = true;
    result.frameCount = frameCount;
    result.samples48k = samplesPerFrame * frameCount;
    result.packetBytes = packetEnd;
    return result;
}

const std::array<std::array<std::uint32_t, 256>, 8>& crcTables() {
    static const std::array<std::array<std::uint32_t, 256>, 8> tables = [] {
        std::array<std::array<std::uint32_t, 256>, 8> result{};
        for (std::uint32_t index = 0; index < result[0].size(); ++index) {
            std::uint32_t value = index << 24;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 0x80000000U) != 0
                    ? (value << 1) ^ 0x04C11DB7U
                    : value << 1;
            }
            result[0][index] = value;
        }
        for (std::size_t table = 1; table < result.size(); ++table) {
            for (std::size_t index = 0; index < result[table].size(); ++index) {
                const std::uint32_t previous = result[table - 1][index];
                result[table][index] =
                    (previous << 8) ^ result[0][previous >> 24];
            }
        }
        return result;
    }();
    return tables;
}

std::uint32_t updateOggCrc(
    std::uint32_t crc,
    const std::uint8_t* data,
    std::size_t size) noexcept {
    const auto& table = crcTables();
    while (size >= 8) {
        const std::uint32_t first = crc ^
            (static_cast<std::uint32_t>(data[0]) << 24) ^
            (static_cast<std::uint32_t>(data[1]) << 16) ^
            (static_cast<std::uint32_t>(data[2]) << 8) ^
            static_cast<std::uint32_t>(data[3]);
        crc =
            table[7][(first >> 24) & 0xFFU] ^
            table[6][(first >> 16) & 0xFFU] ^
            table[5][(first >> 8) & 0xFFU] ^
            table[4][first & 0xFFU] ^
            table[3][data[4]] ^
            table[2][data[5]] ^
            table[1][data[6]] ^
            table[0][data[7]];
        data += 8;
        size -= 8;
    }
    while (size-- != 0) {
        crc = (crc << 8) ^ table[0][((crc >> 24) & 0xFFU) ^ *data++];
    }
    return crc;
}

bool sameFileIdentity(
    const BY_HANDLE_FILE_INFORMATION& left,
    const BY_HANDLE_FILE_INFORMATION& right) noexcept {
    return left.dwVolumeSerialNumber == right.dwVolumeSerialNumber &&
        left.nFileIndexHigh == right.nFileIndexHigh &&
        left.nFileIndexLow == right.nFileIndexLow &&
        left.nFileSizeHigh == right.nFileSizeHigh &&
        left.nFileSizeLow == right.nFileSizeLow &&
        left.ftLastWriteTime.dwHighDateTime == right.ftLastWriteTime.dwHighDateTime &&
        left.ftLastWriteTime.dwLowDateTime == right.ftLastWriteTime.dwLowDateTime;
}

bool isSupportedLocalRegularFile(
    const std::filesystem::path& path,
    std::error_code& error) {
    const std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error || !std::filesystem::is_regular_file(absolute, error) || error) {
        return false;
    }
    const std::filesystem::path root = absolute.root_path();
    if (root.empty()) {
        return false;
    }
    switch (GetDriveTypeW(root.c_str())) {
        case DRIVE_FIXED:
        case DRIVE_REMOVABLE:
        case DRIVE_CDROM:
        case DRIVE_RAMDISK:
            return true;
        default:
            return false;
    }
}

class ForwardFileReader {
public:
    ForwardFileReader(
        const std::filesystem::path& path,
        std::size_t bufferSize,
        std::uint64_t forceReadErrorAfterBytes)
        : buffer_((std::max)(bufferSize, std::size_t{1})),
          forceReadErrorAfterBytes_(forceReadErrorAfterBytes) {
        handle_ = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            reason_ = OggOpusSequentialReason::InputOpenFailed;
            return;
        }
        if (GetFileType(handle_) != FILE_TYPE_DISK ||
            !GetFileInformationByHandle(handle_, &initialInfo_) ||
            (initialInfo_.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            reason_ = OggOpusSequentialReason::InputInfoFailed;
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            return;
        }
        fileSize_ =
            (static_cast<std::uint64_t>(initialInfo_.nFileSizeHigh) << 32) |
            initialInfo_.nFileSizeLow;
        maximumWorkingBufferBytes_ = buffer_.size();
    }

    ~ForwardFileReader() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    ForwardFileReader(const ForwardFileReader&) = delete;
    ForwardFileReader& operator=(const ForwardFileReader&) = delete;

    bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }
    OggOpusSequentialReason reason() const noexcept { return reason_; }
    std::uint64_t fileSize() const noexcept { return fileSize_; }
    std::uint64_t fileIndex() const noexcept {
        return (static_cast<std::uint64_t>(initialInfo_.nFileIndexHigh) << 32) |
            initialInfo_.nFileIndexLow;
    }
    std::uint64_t lastWriteTime100ns() const noexcept {
        return (static_cast<std::uint64_t>(initialInfo_.ftLastWriteTime.dwHighDateTime) << 32) |
            initialInfo_.ftLastWriteTime.dwLowDateTime;
    }
    std::uint32_t volumeSerialNumber() const noexcept {
        return initialInfo_.dwVolumeSerialNumber;
    }
    std::uint64_t bytesReturned() const noexcept { return bytesReturned_; }
    std::uint64_t readCalls() const noexcept { return readCalls_; }
    std::uint64_t position() const noexcept { return logicalPosition_; }
    std::uint64_t maximumWorkingBufferBytes() const noexcept {
        return maximumWorkingBufferBytes_;
    }

    bool readExact(void* output, std::size_t bytes, bool& cleanEof) {
        cleanEof = false;
        auto* destination = static_cast<std::uint8_t*>(output);
        std::size_t copied = 0;
        while (copied < bytes) {
            if (begin_ == end_ && !fill()) {
                cleanEof = copied == 0 && eofObserved_ &&
                    reason_ == OggOpusSequentialReason::NotEligible;
                return false;
            }
            const std::size_t available = end_ - begin_;
            const std::size_t take = (std::min)(available, bytes - copied);
            std::memcpy(destination + copied, buffer_.data() + begin_, take);
            begin_ += take;
            copied += take;
            logicalPosition_ += take;
        }
        return true;
    }

    bool unchanged() const noexcept {
        BY_HANDLE_FILE_INFORMATION finalInfo{};
        return handle_ != INVALID_HANDLE_VALUE &&
            GetFileInformationByHandle(handle_, &finalInfo) &&
            sameFileIdentity(initialInfo_, finalInfo);
    }

private:
    bool fill() {
        if (bytesReturned_ >= forceReadErrorAfterBytes_) {
            reason_ = OggOpusSequentialReason::ForcedReadError;
            return false;
        }
        const std::uint64_t remainingBeforeFault =
            forceReadErrorAfterBytes_ - bytesReturned_;
        const DWORD request = static_cast<DWORD>((std::min<std::uint64_t>)(
            buffer_.size(),
            (std::min<std::uint64_t>)(remainingBeforeFault, MAXDWORD)));
        if (request == 0) {
            reason_ = OggOpusSequentialReason::ForcedReadError;
            return false;
        }
        DWORD actual = 0;
        if (!ReadFile(handle_, buffer_.data(), request, &actual, nullptr)) {
            reason_ = OggOpusSequentialReason::InputReadFailed;
            return false;
        }
        ++readCalls_;
        begin_ = 0;
        end_ = actual;
        bytesReturned_ += actual;
        if (actual == 0) {
            eofObserved_ = true;
            return false;
        }
        return true;
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION initialInfo_{};
    std::vector<std::uint8_t> buffer_;
    std::size_t begin_ = 0;
    std::size_t end_ = 0;
    std::uint64_t forceReadErrorAfterBytes_ =
        (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t fileSize_ = 0;
    std::uint64_t bytesReturned_ = 0;
    std::uint64_t readCalls_ = 0;
    std::uint64_t logicalPosition_ = 0;
    std::uint64_t maximumWorkingBufferBytes_ = 0;
    bool eofObserved_ = false;
    OggOpusSequentialReason reason_ = OggOpusSequentialReason::NotEligible;
};

struct OggPage {
    std::uint64_t offset = 0;
    std::uint8_t flags = 0;
    std::int64_t granule = kUnknownGranule;
    std::uint32_t serial = 0;
    std::uint32_t sequence = 0;
    std::vector<std::uint8_t> segments;
    std::vector<std::uint8_t> body;

    bool continued() const noexcept { return (flags & 0x01U) != 0; }
    bool bos() const noexcept { return (flags & 0x02U) != 0; }
    bool eos() const noexcept { return (flags & 0x04U) != 0; }
};

enum class PageReadStatus { Page, Eof, Error };

PageReadStatus readOggPage(
    ForwardFileReader& reader,
    OggPage& page,
    OggOpusSequentialReason& reason) {
    page = {};
    page.offset = reader.position();
    std::array<std::uint8_t, 27> header{};
    bool cleanEof = false;
    if (!reader.readExact(header.data(), header.size(), cleanEof)) {
        if (cleanEof) {
            return PageReadStatus::Eof;
        }
        reason = reader.reason() == OggOpusSequentialReason::NotEligible
            ? OggOpusSequentialReason::TruncatedPageHeader
            : reader.reason();
        return PageReadStatus::Error;
    }
    if (std::memcmp(header.data(), "OggS", 4) != 0) {
        reason = OggOpusSequentialReason::CapturePatternMissing;
        return PageReadStatus::Error;
    }
    if (header[4] != 0) {
        reason = OggOpusSequentialReason::UnsupportedOggVersion;
        return PageReadStatus::Error;
    }
    if ((header[5] & ~0x07U) != 0) {
        reason = OggOpusSequentialReason::InvalidPageFlags;
        return PageReadStatus::Error;
    }

    page.segments.resize(header[26]);
    if (!page.segments.empty() &&
        !reader.readExact(page.segments.data(), page.segments.size(), cleanEof)) {
        reason = reader.reason() == OggOpusSequentialReason::NotEligible
            ? OggOpusSequentialReason::TruncatedSegmentTable
            : reader.reason();
        return PageReadStatus::Error;
    }
    std::size_t bodyBytes = 0;
    for (const std::uint8_t segment : page.segments) {
        if (bodyBytes > kMaximumOggPageBodyBytes - segment) {
            reason = OggOpusSequentialReason::PageBodySizeOverflow;
            return PageReadStatus::Error;
        }
        bodyBytes += segment;
    }
    page.body.resize(bodyBytes);
    if (!page.body.empty() &&
        !reader.readExact(page.body.data(), page.body.size(), cleanEof)) {
        reason = reader.reason() == OggOpusSequentialReason::NotEligible
            ? OggOpusSequentialReason::TruncatedPageBody
            : reader.reason();
        return PageReadStatus::Error;
    }

    const std::uint32_t storedCrc = little32(header.data() + 22);
    header[22] = header[23] = header[24] = header[25] = 0;
    std::uint32_t crc = updateOggCrc(0, header.data(), header.size());
    crc = updateOggCrc(crc, page.segments.data(), page.segments.size());
    crc = updateOggCrc(crc, page.body.data(), page.body.size());
    if (crc != storedCrc) {
        reason = OggOpusSequentialReason::InvalidPageCrc;
        return PageReadStatus::Error;
    }

    const std::uint64_t rawGranule = little64(header.data() + 6);
    page.flags = header[5];
    if (rawGranule == (std::numeric_limits<std::uint64_t>::max)()) {
        page.granule = kUnknownGranule;
    } else if (rawGranule >
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        page.granule = -2;
    } else {
        page.granule = static_cast<std::int64_t>(rawGranule);
    }
    page.serial = little32(header.data() + 14);
    page.sequence = little32(header.data() + 18);
    return PageReadStatus::Page;
}

struct SerialState {
    bool seen = false;
    bool bosSeen = false;
    bool eosSeen = false;
    bool pageAfterEos = false;
    bool sequenceGap = false;
    bool duplicateBos = false;
    bool pendingOpen = false;
    bool identificationDone = false;
    bool identificationOverflow = false;
    bool isOpus = false;
    bool selected = false;
    std::uint32_t lastSequence = 0;
    std::uint64_t pageCount = 0;
    std::uint64_t packetCount = 0;
    std::uint64_t packetBytes = 0;
    std::vector<std::uint8_t> pending;
    OpusHead opusHead;
};

bool startsWith(
    const std::vector<std::uint8_t>& bytes,
    const char* expected,
    std::size_t size) noexcept {
    return bytes.size() >= size &&
        std::memcmp(bytes.data(), expected, size) == 0;
}

OggOpusSequentialStatus statusForPageReason(
    OggOpusSequentialReason reason) noexcept {
    if (reason == OggOpusSequentialReason::InputReadFailed ||
        reason == OggOpusSequentialReason::ForcedReadError) {
        return OggOpusSequentialStatus::IoError;
    }
    return OggOpusSequentialStatus::InvalidMedia;
}

constexpr std::uintmax_t kMaximumProbeHandoffBytes = 1024U * 1024U;

bool findUniqueJsonValue(
    const std::string& text,
    std::string_view key,
    std::size_t& valueOffset) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t keyOffset = text.find(token);
    if (keyOffset == std::string::npos ||
        text.find(token, keyOffset + token.size()) != std::string::npos) {
        return false;
    }
    std::size_t cursor = keyOffset + token.size();
    while (cursor < text.size() &&
        (text[cursor] == ' ' || text[cursor] == '\t' ||
         text[cursor] == '\r' || text[cursor] == '\n')) {
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor++] != ':') {
        return false;
    }
    while (cursor < text.size() &&
        (text[cursor] == ' ' || text[cursor] == '\t' ||
         text[cursor] == '\r' || text[cursor] == '\n')) {
        ++cursor;
    }
    valueOffset = cursor;
    return cursor < text.size();
}

bool parseJsonUnsigned(
    const std::string& text,
    std::string_view key,
    std::uint64_t& value) {
    std::size_t cursor = 0;
    if (!findUniqueJsonValue(text, key, cursor) ||
        cursor >= text.size() || text[cursor] < '0' || text[cursor] > '9') {
        return false;
    }
    std::uint64_t parsed = 0;
    while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
        const std::uint64_t digit = static_cast<std::uint64_t>(text[cursor] - '0');
        if (parsed > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
        ++cursor;
    }
    value = parsed;
    return true;
}

bool parseJsonBool(
    const std::string& text,
    std::string_view key,
    bool& value) {
    std::size_t cursor = 0;
    if (!findUniqueJsonValue(text, key, cursor)) {
        return false;
    }
    if (text.compare(cursor, 4, "true") == 0) {
        value = true;
        return true;
    }
    if (text.compare(cursor, 5, "false") == 0) {
        value = false;
        return true;
    }
    return false;
}

bool parseJsonString(
    const std::string& text,
    std::string_view key,
    std::string& value) {
    std::size_t cursor = 0;
    if (!findUniqueJsonValue(text, key, cursor) || text[cursor++] != '"') {
        return false;
    }
    std::string parsed;
    while (cursor < text.size()) {
        const unsigned char ch = static_cast<unsigned char>(text[cursor++]);
        if (ch == '"') {
            value = std::move(parsed);
            return true;
        }
        if (ch < 0x20) {
            return false;
        }
        if (ch != '\\') {
            parsed.push_back(static_cast<char>(ch));
            continue;
        }
        if (cursor >= text.size()) {
            return false;
        }
        const char escaped = text[cursor++];
        switch (escaped) {
            case '"': parsed.push_back('"'); break;
            case '\\': parsed.push_back('\\'); break;
            case '/': parsed.push_back('/'); break;
            case 'b': parsed.push_back('\b'); break;
            case 'f': parsed.push_back('\f'); break;
            case 'n': parsed.push_back('\n'); break;
            case 'r': parsed.push_back('\r'); break;
            case 't': parsed.push_back('\t'); break;
            case 'u': {
                if (cursor + 4U > text.size()) {
                    return false;
                }
                unsigned value16 = 0;
                for (int index = 0; index < 4; ++index) {
                    const char hex = text[cursor++];
                    const int digit = hex >= '0' && hex <= '9'
                        ? hex - '0'
                        : hex >= 'a' && hex <= 'f'
                            ? hex - 'a' + 10
                            : hex >= 'A' && hex <= 'F'
                                ? hex - 'A' + 10
                                : -1;
                    if (digit < 0) {
                        return false;
                    }
                    value16 = (value16 << 4) | static_cast<unsigned>(digit);
                }
                if (value16 > 0x7FU) {
                    return false;
                }
                parsed.push_back(static_cast<char>(value16));
                break;
            }
            default: return false;
        }
    }
    return false;
}

bool readProbeDocument(
    const std::filesystem::path& path,
    std::string& text,
    OggOpusSequentialHandoffReason& reason) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        reason = OggOpusSequentialHandoffReason::ProbeDocumentMissing;
        return false;
    }
    if (size == 0 || size > kMaximumProbeHandoffBytes) {
        reason = OggOpusSequentialHandoffReason::ProbeDocumentTooLarge;
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        reason = OggOpusSequentialHandoffReason::ProbeDocumentMissing;
        return false;
    }
    text.resize(static_cast<std::size_t>(size));
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(text.size())) {
        reason = OggOpusSequentialHandoffReason::ProbeDocumentInvalid;
        return false;
    }
    return true;
}

bool currentFileIdentity(
    const std::string& path,
    std::uint64_t& size,
    std::uint64_t& fileIndex,
    std::uint64_t& lastWriteTime,
    std::uint32_t& volumeSerial) noexcept {
    try {
        const std::filesystem::path nativePath = std::filesystem::u8path(path);
        const HANDLE handle = CreateFileW(
            nativePath.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return false;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        const bool valid = GetFileType(handle) == FILE_TYPE_DISK &&
            GetFileInformationByHandle(handle, &info) &&
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
        CloseHandle(handle);
        if (!valid) {
            return false;
        }
        size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) |
            info.nFileSizeLow;
        fileIndex = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) |
            info.nFileIndexLow;
        lastWriteTime =
            (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
            info.ftLastWriteTime.dwLowDateTime;
        volumeSerial = info.dwVolumeSerialNumber;
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

OggOpusPacketDurationResult parseOggOpusPacketDuration(
    const std::uint8_t* packet,
    std::size_t size,
    int streamCount) noexcept {
    OggOpusPacketDurationResult result;
    if (!packet || size == 0 || streamCount <= 0 || streamCount > 255) {
        return result;
    }
    std::size_t cursor = 0;
    int expectedSamples = -1;
    int expectedFrames = 0;
    for (int stream = 0; stream < streamCount; ++stream) {
        const ParsedSubpacket parsed = parseOpusSubpacket(
            packet + cursor,
            size - cursor,
            stream + 1 < streamCount);
        if (!parsed.valid || parsed.packetBytes == 0 ||
            parsed.packetBytes > size - cursor) {
            return result;
        }
        if (expectedSamples >= 0 && parsed.samples48k != expectedSamples) {
            return result;
        }
        expectedSamples = parsed.samples48k;
        expectedFrames = parsed.frameCount;
        cursor += parsed.packetBytes;
    }
    if (cursor != size) {
        return result;
    }
    result.valid = true;
    result.frameCount = expectedFrames;
    result.samples48k = expectedSamples;
    return result;
}

OggOpusSequentialEligibility evaluateOggOpusSequentialEligibility(
    const std::string& path,
    const AVFormatContext* formatContext,
    const AVStream* selectedAudioStream,
    bool strongerSampleExactAuthorityPresent) {
    OggOpusSequentialEligibility result;
    if (path.empty() || !formatContext || !formatContext->iformat ||
        !selectedAudioStream || !selectedAudioStream->codecpar ||
        strongerSampleExactAuthorityPresent ||
        formatContext->nb_streams == 0 ||
        formatContext->nb_streams > kMaximumLogicalStreams ||
        !formatListContains(formatContext->iformat->name, "ogg")) {
        return result;
    }
    const AVCodecParameters* codecpar = selectedAudioStream->codecpar;
    if (codecpar->codec_type != AVMEDIA_TYPE_AUDIO ||
        codecpar->codec_id != AV_CODEC_ID_OPUS ||
        codecpar->sample_rate != kCanonicalOpusSampleRate ||
        codecpar->ch_layout.nb_channels <= 0 ||
        !codecpar->extradata || codecpar->extradata_size <= 0) {
        return result;
    }

    try {
        std::error_code error;
        const std::filesystem::path nativePath = std::filesystem::u8path(path);
        if (!isSupportedLocalRegularFile(nativePath, error)) {
            result.reason = OggOpusSequentialReason::InputNotRegularFile;
            return result;
        }
    } catch (...) {
        result.reason = OggOpusSequentialReason::InputNotRegularFile;
        return result;
    }

    int ordinal = 0;
    bool selectedFound = false;
    for (unsigned index = 0; index < formatContext->nb_streams; ++index) {
        const AVStream* stream = formatContext->streams[index];
        const AVCodecParameters* current = stream ? stream->codecpar : nullptr;
        if (!current || current->codec_type != AVMEDIA_TYPE_AUDIO ||
            current->codec_id != AV_CODEC_ID_OPUS) {
            continue;
        }
        if (stream == selectedAudioStream || stream->index == selectedAudioStream->index) {
            selectedFound = true;
            break;
        }
        ++ordinal;
    }
    if (!selectedFound) {
        result.reason = OggOpusSequentialReason::SelectedStreamMappingConflict;
        return result;
    }

    const auto* begin = codecpar->extradata;
    const std::size_t size = static_cast<std::size_t>(codecpar->extradata_size);
    const OpusHead head = parseOpusHead(begin, size);
    if (!head.valid || head.channels != codecpar->ch_layout.nb_channels ||
        (codecpar->initial_padding > 0 && codecpar->initial_padding != head.preSkip)) {
        result.reason = OggOpusSequentialReason::SelectedStreamMappingConflict;
        return result;
    }

    result.selected.opusStreamOrdinal = ordinal;
    result.selected.logicalStreamCount = formatContext->nb_streams;
    result.selected.channels = head.channels;
    result.selected.preSkip = head.preSkip;
    result.selected.inputSampleRate = head.inputSampleRate;
    result.selected.outputGain = head.outputGain;
    result.selected.mappingFamily = head.mappingFamily;
    result.selected.streamCount = head.streamCount;
    result.selected.coupledCount = head.coupledCount;
    result.selected.opusHead.assign(begin, begin + size);
    result.eligible = true;
    result.reason = OggOpusSequentialReason::CompleteContinuityProof;
    return result;
}

OggOpusSequentialPresentationResult probeOggOpusSequentialPresentation(
    const std::string& path,
    const OggOpusSelectedStreamIdentity& selected,
    const OggOpusSequentialProbeOptions& options) {
    const auto started = std::chrono::steady_clock::now();
    OggOpusSequentialPresentationResult result;
    if (selected.opusStreamOrdinal < 0 || selected.logicalStreamCount == 0 ||
        selected.opusHead.empty() ||
        selected.streamCount == 0) {
        return finishTimedScan(std::move(result), started);
    }
    const OpusHead expectedHead = parseOpusHead(
        selected.opusHead.data(), selected.opusHead.size());
    if (!expectedHead.valid ||
        !matchesIdentity(expectedHead, selected.opusHead, selected)) {
        result.status = OggOpusSequentialStatus::Conflict;
        result.reason = OggOpusSequentialReason::SelectedStreamMappingConflict;
        return finishTimedScan(std::move(result), started);
    }

    std::size_t selectedPacketMaximum = 0;
    if (!deriveMaximumSelectedPacketBytes(selected.streamCount, selectedPacketMaximum)) {
        result.reason = OggOpusSequentialReason::SelectedPacketTooLarge;
        return finishTimedScan(std::move(result), started);
    }
    const std::uint64_t forcedReadError = options.testHooks
        ? options.testHooks->forceReadErrorAfterBytes
        : (std::numeric_limits<std::uint64_t>::max)();
    if (options.testHooks && options.testHooks->selectedPacketMaximumOverride > 0) {
        selectedPacketMaximum = options.testHooks->selectedPacketMaximumOverride;
    }

    std::filesystem::path nativePath;
    try {
        nativePath = std::filesystem::u8path(path);
    } catch (...) {
        result.reason = OggOpusSequentialReason::InputNotRegularFile;
        return finishTimedScan(std::move(result), started);
    }
    ForwardFileReader reader(
        nativePath,
        options.readBufferBytes,
        forcedReadError);
    if (!reader.valid()) {
        result.status = reader.reason() == OggOpusSequentialReason::InputOpenFailed
            ? OggOpusSequentialStatus::IoError
            : OggOpusSequentialStatus::Unsupported;
        result.reason = reader.reason();
        return finishTimedScan(std::move(result), started);
    }

    result.fileSizeBytes = reader.fileSize();
    result.fileIndex = reader.fileIndex();
    result.lastWriteTime100ns = reader.lastWriteTime100ns();
    result.volumeSerialNumber = reader.volumeSerialNumber();
    using SerialEntry = std::pair<std::uint32_t, SerialState>;
    std::vector<SerialEntry> serials;
    const std::size_t maximumPhysicalSerialStates =
        static_cast<std::size_t>(selected.logicalStreamCount) + 1U;
    serials.reserve(maximumPhysicalSerialStates);
    std::optional<std::uint32_t> selectedSerial;
    int nextOpusOrdinal = 0;
    bool anyEosSeen = false;
    bool chained = false;
    bool physicalStreamMappingExceeded = false;
    bool fatal = false;

    auto fail = [&](OggOpusSequentialStatus status, OggOpusSequentialReason reason) {
        if (!fatal) {
            fatal = true;
            result.status = status;
            result.reason = reason;
        }
    };

    while (!fatal) {
        OggPage page;
        OggOpusSequentialReason pageReason = OggOpusSequentialReason::NotEligible;
        const PageReadStatus pageStatus = readOggPage(reader, page, pageReason);
        if (pageStatus == PageReadStatus::Eof) {
            result.reachedPhysicalEof = true;
            break;
        }
        if (pageStatus == PageReadStatus::Error) {
            if (pageReason == OggOpusSequentialReason::InvalidPageCrc) {
                result.allPageCrcValid = false;
            }
            fail(statusForPageReason(pageReason), pageReason);
            break;
        }
        ++result.pagesParsed;

        auto serial = std::find_if(
            serials.begin(), serials.end(), [&](const SerialEntry& entry) {
                return entry.first == page.serial;
            });
        if (serial == serials.end()) {
            if (serials.size() >= maximumPhysicalSerialStates ||
                (serials.size() >= selected.logicalStreamCount &&
                 (!anyEosSeen || !page.bos()))) {
                fail(
                    OggOpusSequentialStatus::Conflict,
                    OggOpusSequentialReason::SelectedStreamMappingConflict);
                break;
            }
            physicalStreamMappingExceeded =
                serials.size() >= selected.logicalStreamCount;
            serials.emplace_back(page.serial, SerialState{});
            serial = std::prev(serials.end());
        }
        SerialState& state = serial->second;
        if (!state.seen) {
            state.seen = true;
            if (!page.bos() || page.sequence != 0) {
                fail(
                    OggOpusSequentialStatus::InvalidMedia,
                    OggOpusSequentialReason::SerialDoesNotStartAtBosSequenceZero);
                break;
            }
        } else {
            const std::uint32_t expectedSequence = state.lastSequence + 1U;
            if (page.sequence != expectedSequence) {
                state.sequenceGap = true;
                result.selectedSequenceContinuous = false;
                fail(
                    OggOpusSequentialStatus::Conflict,
                    OggOpusSequentialReason::SerialSequenceGap);
                break;
            }
        }
        if (page.bos()) {
            if (state.bosSeen) {
                state.duplicateBos = true;
                fail(
                    OggOpusSequentialStatus::Conflict,
                    OggOpusSequentialReason::DuplicateSerialBos);
                break;
            }
            state.bosSeen = true;
        }
        if (state.eosSeen) {
            state.pageAfterEos = true;
            fail(
                OggOpusSequentialStatus::Conflict,
                OggOpusSequentialReason::SerialPageAfterEos);
            break;
        }
        if (page.continued() != state.pendingOpen) {
            result.packetContinuityValid = false;
            fail(
                OggOpusSequentialStatus::Conflict,
                OggOpusSequentialReason::PacketContinuationMismatch);
            break;
        }
        state.lastSequence = page.sequence;
        ++state.pageCount;

        std::size_t bodyCursor = 0;
        for (const std::uint8_t segment : page.segments) {
            if (bodyCursor > page.body.size() ||
                segment > page.body.size() - bodyCursor) {
                fail(
                    OggOpusSequentialStatus::InvalidMedia,
                    OggOpusSequentialReason::TruncatedPageBody);
                break;
            }
            if (!state.pendingOpen) {
                state.packetBytes = 0;
                state.pending.clear();
                state.identificationOverflow = false;
            }
            std::uint64_t newPacketBytes = 0;
            if (!checkedAdd(state.packetBytes, segment, newPacketBytes)) {
                fail(
                    OggOpusSequentialStatus::InvalidMedia,
                    OggOpusSequentialReason::SelectedPacketTooLarge);
                break;
            }
            state.packetBytes = newPacketBytes;

            const auto segmentBegin =
                page.body.begin() + static_cast<std::ptrdiff_t>(bodyCursor);
            const auto segmentEnd =
                page.body.begin() + static_cast<std::ptrdiff_t>(bodyCursor + segment);
            if (!state.identificationDone) {
                const std::size_t available = state.pending.size() < kMaximumOpusHeadBytes
                    ? kMaximumOpusHeadBytes - state.pending.size()
                    : 0;
                const std::size_t take = (std::min<std::size_t>)(available, segment);
                state.pending.insert(
                    state.pending.end(), segmentBegin, segmentBegin + take);
                if (take != segment) {
                    state.identificationOverflow = true;
                }
            } else if (state.selected && state.packetCount == 1) {
                const std::size_t available = state.pending.size() < 8
                    ? 8 - state.pending.size()
                    : 0;
                const std::size_t take = (std::min<std::size_t>)(available, segment);
                state.pending.insert(
                    state.pending.end(), segmentBegin, segmentBegin + take);
            } else if (state.selected) {
                if (state.packetBytes > selectedPacketMaximum) {
                    fail(
                        OggOpusSequentialStatus::Unsupported,
                        OggOpusSequentialReason::SelectedPacketTooLarge);
                    break;
                }
                state.pending.insert(state.pending.end(), segmentBegin, segmentEnd);
            }
            bodyCursor += segment;
            state.pendingOpen = segment == 255;
            if (state.pendingOpen) {
                continue;
            }

            ++state.packetCount;
            if (!state.identificationDone) {
                state.identificationDone = true;
                if (startsWith(state.pending, "OpusHead", 8)) {
                    state.isOpus = true;
                    if (state.identificationOverflow ||
                        state.packetBytes != state.pending.size()) {
                        fail(
                            OggOpusSequentialStatus::InvalidMedia,
                            OggOpusSequentialReason::IdentificationPacketTooLarge);
                        break;
                    }
                    state.opusHead = parseOpusHead(
                        state.pending.data(), state.pending.size());
                    if (!state.opusHead.valid) {
                        fail(
                            OggOpusSequentialStatus::InvalidMedia,
                            state.opusHead.reason);
                        break;
                    }
                    if (anyEosSeen) {
                        chained = true;
                    }
                    const int ordinal = nextOpusOrdinal++;
                    if (ordinal == selected.opusStreamOrdinal) {
                        if (!matchesIdentity(state.opusHead, state.pending, selected)) {
                            fail(
                                OggOpusSequentialStatus::Conflict,
                                OggOpusSequentialReason::SelectedStreamMappingConflict);
                            break;
                        }
                        state.selected = true;
                        selectedSerial = page.serial;
                        result.selectedSerial = page.serial;
                        result.preSkip = state.opusHead.preSkip;
                    }
                }
            }

            if (state.selected) {
                ++result.selectedPackets;
                result.maximumPacketBytes = (std::max)(
                    result.maximumPacketBytes, state.packetBytes);
                if (state.packetCount == 1) {
                    if (!startsWith(state.pending, "OpusHead", 8)) {
                        fail(
                            OggOpusSequentialStatus::Conflict,
                            OggOpusSequentialReason::SelectedStreamMappingConflict);
                        break;
                    }
                } else if (state.packetCount == 2) {
                    if (!startsWith(state.pending, "OpusTags", 8)) {
                        fail(
                            OggOpusSequentialStatus::InvalidMedia,
                            OggOpusSequentialReason::SelectedOpusTagsMissing);
                        break;
                    }
                } else {
                    const OggOpusPacketDurationResult duration =
                        parseOggOpusPacketDuration(
                            state.pending.data(),
                            state.pending.size(),
                            state.opusHead.streamCount);
                    if (!duration.valid || duration.samples48k <= 0) {
                        fail(
                            OggOpusSequentialStatus::InvalidMedia,
                            OggOpusSequentialReason::InvalidOpusPacket);
                        break;
                    }
                    std::uint64_t total = 0;
                    if (!checkedAdd(
                            result.physicalPacketFrames,
                            static_cast<std::uint64_t>(duration.samples48k),
                            total)) {
                        fail(
                            OggOpusSequentialStatus::Conflict,
                            OggOpusSequentialReason::PacketDurationOverflow);
                        break;
                    }
                    result.physicalPacketFrames = total;
                    result.lastPacketDuration =
                        static_cast<std::uint64_t>(duration.samples48k);
                    ++result.selectedAudioPackets;
                }
            }
            state.pending.clear();
        }
        if (fatal) {
            break;
        }
        if (page.eos()) {
            state.eosSeen = true;
            anyEosSeen = true;
            if (state.selected) {
                if (page.granule < 0) {
                    result.eosGranule = 0;
                    result.selectedEosGranuleKnown = false;
                } else {
                    result.eosGranule = static_cast<std::uint64_t>(page.granule);
                    result.selectedEosGranuleKnown = true;
                }
            }
        }
    }

    result.bytesReturned = reader.bytesReturned();
    result.uniqueBytes = result.bytesReturned;
    result.duplicateBytes = 0;
    result.readCalls = reader.readCalls();
    result.seekCallsAfterOpen = 0;
    std::size_t serialStateBytes = 0;
    const std::size_t bytesPerSerial = sizeof(SerialEntry) + kMaximumOpusHeadBytes;
    const bool serialStateBoundValid = checkedMultiply(
        bytesPerSerial,
        maximumPhysicalSerialStates,
        serialStateBytes);
    std::uint64_t maximumWorking = reader.maximumWorkingBufferBytes();
    const bool workingBoundValid =
        serialStateBoundValid &&
        checkedAdd(maximumWorking, kMaximumOggPageBodyBytes, maximumWorking) &&
        checkedAdd(maximumWorking, 255U, maximumWorking) &&
        checkedAdd(maximumWorking, selectedPacketMaximum, maximumWorking) &&
        checkedAdd(maximumWorking, serialStateBytes, maximumWorking);
    result.maximumWorkingBufferBytes = workingBoundValid ? maximumWorking : 0;
    if (!workingBoundValid && !fatal) {
        fail(
            OggOpusSequentialStatus::Unsupported,
            OggOpusSequentialReason::SelectedPacketTooLarge);
    }

    for (const auto& entry : serials) {
        if (entry.second.pendingOpen && !fatal) {
            result.packetContinuityValid = false;
            fail(
                OggOpusSequentialStatus::InvalidMedia,
                OggOpusSequentialReason::IncompletePacketAtEof);
            break;
        }
    }
    if (!reader.unchanged() && !fatal) {
        fail(
            OggOpusSequentialStatus::Conflict,
            OggOpusSequentialReason::InputChangedDuringScan);
    }
    if (!fatal &&
        (!result.reachedPhysicalEof ||
         result.bytesReturned != result.fileSizeBytes)) {
        fail(
            OggOpusSequentialStatus::IoError,
            OggOpusSequentialReason::PhysicalEofNotReached);
    }
    if (!fatal && !selectedSerial) {
        fail(
            OggOpusSequentialStatus::Conflict,
            OggOpusSequentialReason::SelectedOpusStreamAbsent);
    }
    if (!fatal && chained) {
        fail(
            OggOpusSequentialStatus::Chained,
            OggOpusSequentialReason::ChainedOpusUnsupported);
    }
    if (!fatal && physicalStreamMappingExceeded) {
        fail(
            OggOpusSequentialStatus::Conflict,
            OggOpusSequentialReason::SelectedStreamMappingConflict);
    }
    if (!fatal) {
        const auto selectedState = std::find_if(
            serials.begin(), serials.end(), [&](const SerialEntry& entry) {
                return entry.first == *selectedSerial;
            });
        if (selectedState == serials.end()) {
            fail(
                OggOpusSequentialStatus::Conflict,
                OggOpusSequentialReason::SelectedStreamMappingConflict);
            return finishTimedScan(std::move(result), started);
        }
        const SerialState& state = selectedState->second;
        result.selectedPages = state.pageCount;
        if (!state.eosSeen) {
            fail(
                OggOpusSequentialStatus::Conflict,
                OggOpusSequentialReason::SelectedEosMissing);
        } else if (!result.selectedEosGranuleKnown) {
            fail(
                OggOpusSequentialStatus::InvalidMedia,
                OggOpusSequentialReason::UnknownSelectedEosGranule);
        } else if (result.eosGranule < result.preSkip) {
            fail(
                OggOpusSequentialStatus::InvalidMedia,
                OggOpusSequentialReason::EosGranuleBeforePreSkip);
        } else if (result.selectedAudioPackets == 0 ||
            result.lastPacketDuration == 0) {
            fail(
                OggOpusSequentialStatus::InvalidMedia,
                OggOpusSequentialReason::SelectedAudioPacketsMissing);
        } else if (result.physicalPacketFrames < result.eosGranule) {
            fail(
                OggOpusSequentialStatus::Conflict,
                OggOpusSequentialReason::EosGranuleOutsideFinalPacketInterval);
        } else {
            result.terminalDiscard =
                result.physicalPacketFrames - result.eosGranule;
            result.finalGranuleInPacketInterval =
                result.terminalDiscard < result.lastPacketDuration;
            if (!result.finalGranuleInPacketInterval) {
                fail(
                    OggOpusSequentialStatus::Conflict,
                    OggOpusSequentialReason::EosGranuleOutsideFinalPacketInterval);
            } else {
                result.presentationFrames = result.eosGranule - result.preSkip;
                if (result.presentationFrames == 0) {
                    fail(
                        OggOpusSequentialStatus::InvalidMedia,
                        OggOpusSequentialReason::EmptyPresentation);
                } else {
                    result.status = OggOpusSequentialStatus::Exact;
                    result.reason = OggOpusSequentialReason::CompleteContinuityProof;
                }
            }
        }
    }
    return finishTimedScan(std::move(result), started);
}

TotalPresentationEvidence makeOggOpusSequentialTotalPresentationEvidence(
    const OggOpusSequentialPresentationResult& result) noexcept {
    TotalPresentationEvidence evidence;
    if (!result.exact() || result.presentationFrames == 0) {
        return evidence;
    }
    evidence.frames = result.presentationFrames;
    evidence.trust = PresentationTotalTrust::SampleExact;
    evidence.source = PresentationTotalSource::OggOpusSequentialPresentation;
    evidence.domain = PresentationSampleDomain::NativeStreamSamples;
    evidence.sampleRate = kCanonicalOpusSampleRate;
    evidence.exactRescale = true;
    evidence.validation = PresentationTotalValidation::SelfContainedMetadata;
    return evidence;
}

OggOpusSequentialHandoffResult readOggOpusSequentialPresentationHandoff(
    const std::filesystem::path& probeJsonPath,
    const std::string& sourcePath,
    const AVFormatContext* formatContext,
    const AVStream* selectedAudioStream,
    const TotalPresentationEvidence& independentEvidence) noexcept {
    OggOpusSequentialHandoffResult result;
    try {
        const OggOpusSequentialEligibility eligibility =
            evaluateOggOpusSequentialEligibility(
                sourcePath, formatContext, selectedAudioStream, false);
        if (!eligibility.eligible) {
            result.reason = OggOpusSequentialHandoffReason::SelectedStreamMismatch;
            return result;
        }

        std::string document;
        if (!readProbeDocument(probeJsonPath, document, result.reason)) {
            return result;
        }

        std::string recordedSourcePath;
        std::string recordedStatus;
        std::string recordedReason;
        std::string recordedEvidenceSource;
        std::uint64_t selectedStreamIndex = 0;
        std::uint64_t volumeSerial = 0;
        bool genericScanSkipped = false;
        OggOpusSequentialPresentationResult& presentation = result.presentation;
        bool fieldsValid =
            parseJsonString(document, "sourcePath", recordedSourcePath) &&
            parseJsonString(
                document, "oggOpusSequentialStatus", recordedStatus) &&
            parseJsonString(
                document, "oggOpusSequentialReason", recordedReason) &&
            parseJsonString(
                document, "decodedSampleFramesSource", recordedEvidenceSource) &&
            parseJsonUnsigned(
                document, "selectedAudioStreamIndex", selectedStreamIndex);
        std::uint64_t selectedSerial = 0;
        fieldsValid = fieldsValid &&
            parseJsonUnsigned(
                document, "oggOpusSequentialSelectedSerial", selectedSerial) &&
            parseJsonUnsigned(
                document, "oggOpusSequentialPreSkip", presentation.preSkip) &&
            parseJsonUnsigned(
                document, "oggOpusSequentialPhysicalPacketFrames",
                presentation.physicalPacketFrames) &&
            parseJsonUnsigned(
                document, "oggOpusSequentialLastPacketDuration",
                presentation.lastPacketDuration) &&
            parseJsonUnsigned(
                document, "oggOpusSequentialEosGranule", presentation.eosGranule) &&
            parseJsonUnsigned(
                document, "oggOpusSequentialTerminalDiscard",
                presentation.terminalDiscard) &&
            parseJsonUnsigned(
                document, "oggOpusSequentialPresentationFrames",
                presentation.presentationFrames) &&
            parseJsonUnsigned(
                document, "oggOpusSequentialFileSizeBytes",
                presentation.fileSizeBytes) &&
            parseJsonUnsigned(
                document, "oggOpusSequentialFileIndex", presentation.fileIndex) &&
            parseJsonUnsigned(
                document, "oggOpusSequentialLastWriteTime100ns",
                presentation.lastWriteTime100ns) &&
            parseJsonUnsigned(
                document, "oggOpusSequentialVolumeSerialNumber", volumeSerial) &&
            parseJsonUnsigned(
                document, "oggOpusSequentialBytesReturned",
                presentation.bytesReturned) &&
            parseJsonUnsigned(
                document, "oggOpusSequentialUniqueBytes", presentation.uniqueBytes) &&
            parseJsonUnsigned(
                document, "oggOpusSequentialDuplicateBytes",
                presentation.duplicateBytes) &&
            parseJsonUnsigned(
                document, "oggOpusSequentialReadCalls", presentation.readCalls) &&
            parseJsonUnsigned(
                document, "oggOpusSequentialSeekCallsAfterOpen",
                presentation.seekCallsAfterOpen) &&
            parseJsonUnsigned(
                document, "oggOpusSequentialScanDurationUs",
                presentation.scanDurationUs) &&
            parseJsonUnsigned(
                document, "oggOpusSequentialPagesParsed", presentation.pagesParsed) &&
            parseJsonUnsigned(
                document, "oggOpusSequentialSelectedPages",
                presentation.selectedPages) &&
            parseJsonUnsigned(
                document, "oggOpusSequentialSelectedAudioPackets",
                presentation.selectedAudioPackets) &&
            parseJsonUnsigned(
                document, "oggOpusSequentialMaximumPacketBytes",
                presentation.maximumPacketBytes) &&
            parseJsonUnsigned(
                document, "oggOpusSequentialMaximumWorkingBufferBytes",
                presentation.maximumWorkingBufferBytes) &&
            parseJsonBool(
                document, "oggOpusSequentialReachedEof",
                presentation.reachedPhysicalEof) &&
            parseJsonBool(
                document, "oggOpusSequentialAllPageCrcValid",
                presentation.allPageCrcValid) &&
            parseJsonBool(
                document, "oggOpusSequentialSelectedSequenceContinuous",
                presentation.selectedSequenceContinuous) &&
            parseJsonBool(
                document, "oggOpusSequentialPacketContinuityValid",
                presentation.packetContinuityValid) &&
            parseJsonBool(
                document, "oggOpusSequentialFinalGranuleInPacketInterval",
                presentation.finalGranuleInPacketInterval) &&
            parseJsonBool(
                document, "oggOpusSequentialGenericScanSkipped",
                genericScanSkipped);

        if (!fieldsValid || recordedSourcePath != sourcePath ||
            recordedStatus != "exact" ||
            recordedReason != oggOpusSequentialReasonName(
                OggOpusSequentialReason::CompleteContinuityProof) ||
            recordedEvidenceSource != "ogg_opus_sequential_presentation" ||
            selectedStreamIndex !=
                static_cast<std::uint64_t>(selectedAudioStream->index) ||
            selectedSerial > (std::numeric_limits<std::uint32_t>::max)() ||
            volumeSerial > (std::numeric_limits<std::uint32_t>::max)() ||
            !genericScanSkipped) {
            result.reason = OggOpusSequentialHandoffReason::ProbeDocumentInvalid;
            return result;
        }
        presentation.selectedSerial = static_cast<std::uint32_t>(selectedSerial);
        presentation.volumeSerialNumber = static_cast<std::uint32_t>(volumeSerial);

        std::uint64_t currentSize = 0;
        std::uint64_t currentIndex = 0;
        std::uint64_t currentWriteTime = 0;
        std::uint32_t currentVolume = 0;
        if (!currentFileIdentity(
                sourcePath,
                currentSize,
                currentIndex,
                currentWriteTime,
                currentVolume) ||
            currentSize != presentation.fileSizeBytes ||
            currentIndex != presentation.fileIndex ||
            currentWriteTime != presentation.lastWriteTime100ns ||
            currentVolume != presentation.volumeSerialNumber) {
            result.status = OggOpusSequentialHandoffStatus::Conflict;
            result.reason = OggOpusSequentialHandoffReason::SourceIdentityMismatch;
            return result;
        }

        const bool internallyExact =
            presentation.presentationFrames > 0 &&
            presentation.fileSizeBytes > 0 &&
            presentation.bytesReturned == presentation.fileSizeBytes &&
            presentation.uniqueBytes == presentation.fileSizeBytes &&
            presentation.duplicateBytes == 0 &&
            presentation.readCalls > 0 &&
            presentation.seekCallsAfterOpen == 0 &&
            presentation.pagesParsed > 0 &&
            presentation.selectedPages > 0 &&
            presentation.selectedAudioPackets > 0 &&
            presentation.reachedPhysicalEof &&
            presentation.allPageCrcValid &&
            presentation.selectedSequenceContinuous &&
            presentation.packetContinuityValid &&
            presentation.finalGranuleInPacketInterval &&
            presentation.eosGranule >= presentation.preSkip &&
            presentation.physicalPacketFrames >= presentation.eosGranule &&
            presentation.terminalDiscard ==
                presentation.physicalPacketFrames - presentation.eosGranule &&
            presentation.lastPacketDuration > presentation.terminalDiscard &&
            presentation.presentationFrames ==
                presentation.eosGranule - presentation.preSkip;
        if (!internallyExact) {
            result.reason = OggOpusSequentialHandoffReason::ProbeDocumentInvalid;
            return result;
        }

        presentation.status = OggOpusSequentialStatus::Exact;
        presentation.reason = OggOpusSequentialReason::CompleteContinuityProof;
        presentation.selectedEosGranuleKnown = true;
        result.evidence =
            makeOggOpusSequentialTotalPresentationEvidence(presentation);
        const bool independentExact =
            independentEvidence.trust == PresentationTotalTrust::SampleExact &&
            independentEvidence.domain == PresentationSampleDomain::NativeStreamSamples &&
            independentEvidence.sampleRate > 0 &&
            independentEvidence.exactRescale &&
            !independentEvidence.conflict;
        if (independentExact &&
            (independentEvidence.sampleRate != result.evidence.sampleRate ||
             independentEvidence.frames != result.evidence.frames)) {
            result.status = OggOpusSequentialHandoffStatus::Conflict;
            result.reason = OggOpusSequentialHandoffReason::EvidenceConflict;
            result.evidence = {};
            return result;
        }

        result.status = OggOpusSequentialHandoffStatus::Accepted;
        result.reason = OggOpusSequentialHandoffReason::AcceptedFastProbeEvidence;
        return result;
    } catch (...) {
        result = {};
        result.reason = OggOpusSequentialHandoffReason::ProbeDocumentInvalid;
        return result;
    }
}

const char* oggOpusSequentialStatusName(OggOpusSequentialStatus status) noexcept {
    switch (status) {
        case OggOpusSequentialStatus::Exact: return "exact";
        case OggOpusSequentialStatus::Unsupported: return "unsupported";
        case OggOpusSequentialStatus::Chained: return "chained";
        case OggOpusSequentialStatus::Conflict: return "conflict";
        case OggOpusSequentialStatus::InvalidMedia: return "invalid_media";
        case OggOpusSequentialStatus::IoError: return "io_error";
    }
    return "unsupported";
}

const char* oggOpusSequentialReasonName(OggOpusSequentialReason reason) noexcept {
    switch (reason) {
        case OggOpusSequentialReason::NotEligible: return "not_eligible";
        case OggOpusSequentialReason::CompleteContinuityProof:
            return "complete_page_packet_and_granule_proof";
        case OggOpusSequentialReason::InputNotRegularFile: return "input_not_regular_file";
        case OggOpusSequentialReason::InputOpenFailed: return "input_open_failed";
        case OggOpusSequentialReason::InputInfoFailed: return "input_info_failed";
        case OggOpusSequentialReason::InputReadFailed: return "input_read_failed";
        case OggOpusSequentialReason::InputChangedDuringScan: return "input_changed_during_scan";
        case OggOpusSequentialReason::PhysicalEofNotReached: return "physical_eof_not_reached";
        case OggOpusSequentialReason::TruncatedPageHeader: return "truncated_page_header";
        case OggOpusSequentialReason::CapturePatternMissing: return "capture_pattern_missing";
        case OggOpusSequentialReason::UnsupportedOggVersion: return "unsupported_ogg_version";
        case OggOpusSequentialReason::InvalidPageFlags: return "invalid_page_flags";
        case OggOpusSequentialReason::TruncatedSegmentTable: return "truncated_segment_table";
        case OggOpusSequentialReason::PageBodySizeOverflow: return "page_body_size_overflow";
        case OggOpusSequentialReason::TruncatedPageBody: return "truncated_page_body";
        case OggOpusSequentialReason::InvalidPageCrc: return "invalid_page_crc";
        case OggOpusSequentialReason::SerialDoesNotStartAtBosSequenceZero:
            return "serial_does_not_start_at_bos_sequence_zero";
        case OggOpusSequentialReason::SerialSequenceGap: return "serial_sequence_gap";
        case OggOpusSequentialReason::DuplicateSerialBos: return "duplicate_serial_bos";
        case OggOpusSequentialReason::SerialPageAfterEos: return "serial_page_after_eos";
        case OggOpusSequentialReason::PacketContinuationMismatch:
            return "packet_continuation_mismatch";
        case OggOpusSequentialReason::IdentificationPacketTooLarge:
            return "identification_packet_too_large";
        case OggOpusSequentialReason::InvalidOpusHead: return "invalid_opus_head";
        case OggOpusSequentialReason::UnsupportedOpusHeadVersion:
            return "unsupported_opus_head_version";
        case OggOpusSequentialReason::SelectedStreamMappingConflict:
            return "selected_stream_mapping_conflict";
        case OggOpusSequentialReason::SelectedOpusStreamAbsent:
            return "selected_opus_stream_absent";
        case OggOpusSequentialReason::SelectedOpusTagsMissing:
            return "selected_opus_tags_missing";
        case OggOpusSequentialReason::SelectedPacketTooLarge: return "selected_packet_too_large";
        case OggOpusSequentialReason::InvalidOpusPacket: return "invalid_opus_packet";
        case OggOpusSequentialReason::PacketDurationOverflow: return "packet_duration_overflow";
        case OggOpusSequentialReason::IncompletePacketAtEof:
            return "incomplete_packet_at_eof";
        case OggOpusSequentialReason::ChainedOpusUnsupported:
            return "chained_opus_unsupported";
        case OggOpusSequentialReason::SelectedEosMissing: return "selected_eos_missing";
        case OggOpusSequentialReason::UnknownSelectedEosGranule:
            return "unknown_selected_eos_granule";
        case OggOpusSequentialReason::EosGranuleBeforePreSkip:
            return "eos_granule_before_pre_skip";
        case OggOpusSequentialReason::SelectedAudioPacketsMissing:
            return "selected_audio_packets_missing";
        case OggOpusSequentialReason::EosGranuleOutsideFinalPacketInterval:
            return "eos_granule_outside_final_packet_interval";
        case OggOpusSequentialReason::EmptyPresentation: return "empty_presentation";
        case OggOpusSequentialReason::IndependentExactAuthorityConflict:
            return "independent_exact_authority_conflict";
        case OggOpusSequentialReason::ForcedReadError: return "forced_read_error";
    }
    return "not_eligible";
}

const char* oggOpusSequentialHandoffStatusName(
    OggOpusSequentialHandoffStatus status) noexcept {
    switch (status) {
        case OggOpusSequentialHandoffStatus::Accepted: return "accepted";
        case OggOpusSequentialHandoffStatus::Unavailable: return "unavailable";
        case OggOpusSequentialHandoffStatus::Conflict: return "conflict";
    }
    return "unavailable";
}

const char* oggOpusSequentialHandoffReasonName(
    OggOpusSequentialHandoffReason reason) noexcept {
    switch (reason) {
        case OggOpusSequentialHandoffReason::AcceptedFastProbeEvidence:
            return "accepted_fast_probe_evidence";
        case OggOpusSequentialHandoffReason::ProbeDocumentMissing:
            return "probe_document_missing";
        case OggOpusSequentialHandoffReason::ProbeDocumentTooLarge:
            return "probe_document_too_large";
        case OggOpusSequentialHandoffReason::ProbeDocumentInvalid:
            return "probe_document_invalid";
        case OggOpusSequentialHandoffReason::SourceIdentityMismatch:
            return "source_identity_mismatch";
        case OggOpusSequentialHandoffReason::SelectedStreamMismatch:
            return "selected_stream_mismatch";
        case OggOpusSequentialHandoffReason::EvidenceConflict:
            return "evidence_conflict";
    }
    return "probe_document_invalid";
}

}  // namespace AveMediaBridge::Probe
