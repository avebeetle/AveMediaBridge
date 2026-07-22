#include "Mp4Mp3SampleEditTablePresentation.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace AveMediaBridge::Probe {
namespace {

constexpr std::size_t kMaximumTopLevelBoxes = 4096;
constexpr std::size_t kMaximumParsedBoxes = 8192;
constexpr std::size_t kMaximumTracks = 256;
constexpr std::size_t kMaximumTableEntries = 1U << 20;
constexpr std::size_t kMaximumSttsEntries = 65536;
constexpr std::size_t kMaximumProbeDocumentBytes = 2U * 1024U * 1024U;

constexpr std::uint32_t fourcc(char a, char b, char c, char d) noexcept {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8) |
        static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

struct Failure final : std::runtime_error {
    Failure(Mp4Mp3SampleTableStatus failureStatus,
            Mp4Mp3SampleTableReason failureReason)
        : std::runtime_error("mp4/mp3 table proof failed"),
          status(failureStatus), reason(failureReason) {}

    Mp4Mp3SampleTableStatus status;
    Mp4Mp3SampleTableReason reason;
};

[[noreturn]] void fail(
    Mp4Mp3SampleTableStatus status,
    Mp4Mp3SampleTableReason reason) {
    throw Failure(status, reason);
}

bool checkedAdd(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& result) noexcept {
    if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
        return false;
    }
    result = left + right;
    return true;
}

bool checkedMultiply(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& result) noexcept {
    if (right != 0 && left > (std::numeric_limits<std::uint64_t>::max)() / right) {
        return false;
    }
    result = left * right;
    return true;
}

std::uint16_t be16(const std::uint8_t* data) noexcept {
    return static_cast<std::uint16_t>(data[0] << 8U | data[1]);
}

std::uint32_t be32(const std::uint8_t* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
        (static_cast<std::uint32_t>(data[1]) << 16U) |
        (static_cast<std::uint32_t>(data[2]) << 8U) |
        static_cast<std::uint32_t>(data[3]);
}

std::uint64_t be64(const std::uint8_t* data) noexcept {
    return (static_cast<std::uint64_t>(be32(data)) << 32U) | be32(data + 4);
}

std::int32_t bes32(const std::uint8_t* data) noexcept {
    return static_cast<std::int32_t>(be32(data));
}

std::int64_t bes64(const std::uint8_t* data) noexcept {
    return static_cast<std::int64_t>(be64(data));
}

struct FileIdentity {
    std::uint64_t size = 0;
    std::uint64_t index = 0;
    std::uint64_t lastWriteTime = 0;
    std::uint32_t volume = 0;
};

bool readFileIdentity(const std::string& path, FileIdentity& identity) noexcept {
    try {
        const std::filesystem::path nativePath = std::filesystem::u8path(path);
        const HANDLE handle = CreateFileW(
            nativePath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
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
        identity.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) |
            info.nFileSizeLow;
        identity.index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) |
            info.nFileIndexLow;
        identity.lastWriteTime =
            (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32U) |
            info.ftLastWriteTime.dwLowDateTime;
        identity.volume = info.dwVolumeSerialNumber;
        return identity.size > 0;
    } catch (...) {
        return false;
    }
}

struct ReadRange {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
};

class BoundedReader {
public:
    BoundedReader(
        const std::string& path,
        const Mp4Mp3SampleTableProbeOptions& options)
        : path_(path), options_(options) {
        if (!readFileIdentity(path, identity_)) {
            fail(Mp4Mp3SampleTableStatus::UnsupportedEarly,
                 Mp4Mp3SampleTableReason::InputNotRegularFile);
        }
        input_.open(std::filesystem::u8path(path), std::ios::binary);
        if (!input_) {
            fail(Mp4Mp3SampleTableStatus::IoError,
                 Mp4Mp3SampleTableReason::InputOpenFailed);
        }
        ranges_.reserve(64);
    }

    const FileIdentity& identity() const noexcept { return identity_; }
    std::uint64_t size() const noexcept { return identity_.size; }
    std::uint64_t bytesReturned() const noexcept { return bytesReturned_; }
    std::uint64_t readCalls() const noexcept { return readCalls_; }
    std::uint64_t seekCalls() const noexcept { return seekCalls_; }
    std::uint64_t maximumOffsetReached() const noexcept { return maximumOffset_; }

    std::vector<std::uint8_t> readAt(std::uint64_t offset, std::uint64_t size) {
        std::vector<std::uint8_t> bytes;
        if (size > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
            fail(Mp4Mp3SampleTableStatus::InvalidMedia,
                 Mp4Mp3SampleTableReason::InvalidBox);
        }
        bytes.resize(static_cast<std::size_t>(size));
        readAt(offset, bytes.data(), bytes.size());
        return bytes;
    }

    void readAt(std::uint64_t offset, void* destination, std::size_t size) {
        if (offset > identity_.size || size > identity_.size - offset) {
            fail(Mp4Mp3SampleTableStatus::InvalidMedia,
                 Mp4Mp3SampleTableReason::InvalidBox);
        }
        const std::uint64_t amount = static_cast<std::uint64_t>(size);
        if (amount > options_.hardReadBudgetBytes -
                std::min(options_.hardReadBudgetBytes, bytesReturned_)) {
            fail(Mp4Mp3SampleTableStatus::IoBudgetExceeded,
                 Mp4Mp3SampleTableReason::IoBudgetExceeded);
        }
        if (bytesReturned_ >= options_.forceReadErrorAfterBytes ||
            amount > options_.forceReadErrorAfterBytes -
                std::min(options_.forceReadErrorAfterBytes, bytesReturned_)) {
            fail(Mp4Mp3SampleTableStatus::IoError,
                 Mp4Mp3SampleTableReason::InputReadFailed);
        }
        if (position_ != offset) {
            if (options_.forceSeekFailure ||
                offset > static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)())) {
                fail(Mp4Mp3SampleTableStatus::IoError,
                     Mp4Mp3SampleTableReason::InputSeekFailed);
            }
            input_.clear();
            input_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            ++seekCalls_;
            if (!input_) {
                fail(Mp4Mp3SampleTableStatus::IoError,
                     Mp4Mp3SampleTableReason::InputSeekFailed);
            }
            position_ = offset;
        }
        if (size > 0) {
            input_.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
            ++readCalls_;
            const std::size_t actual = static_cast<std::size_t>(input_.gcount());
            bytesReturned_ += actual;
            position_ += actual;
            maximumOffset_ = std::max(maximumOffset_, offset + actual);
            if (actual > 0) {
                if (ranges_.size() >= kMaximumTopLevelBoxes + 8U) {
                    fail(Mp4Mp3SampleTableStatus::UnsupportedLate,
                         Mp4Mp3SampleTableReason::ParserResourceLimit);
                }
                ranges_.push_back({offset, offset + actual});
            }
            if (actual != size) {
                fail(Mp4Mp3SampleTableStatus::IoError,
                     Mp4Mp3SampleTableReason::InputReadFailed);
            }
        }
    }

    std::uint64_t uniqueBytes() const {
        std::vector<ReadRange> ranges = ranges_;
        std::sort(ranges.begin(), ranges.end(), [](const auto& left, const auto& right) {
            return left.start < right.start;
        });
        std::uint64_t total = 0;
        std::uint64_t start = 0;
        std::uint64_t end = 0;
        bool present = false;
        for (const ReadRange& range : ranges) {
            if (!present || range.start > end) {
                if (present) {
                    total += end - start;
                }
                start = range.start;
                end = range.end;
                present = true;
            } else {
                end = std::max(end, range.end);
            }
        }
        if (present) {
            total += end - start;
        }
        return total;
    }

    bool unchanged() const noexcept {
        FileIdentity current;
        return readFileIdentity(path_, current) &&
            current.size == identity_.size && current.index == identity_.index &&
            current.lastWriteTime == identity_.lastWriteTime &&
            current.volume == identity_.volume;
    }

private:
    std::string path_;
    Mp4Mp3SampleTableProbeOptions options_;
    FileIdentity identity_;
    std::ifstream input_;
    std::uint64_t position_ = 0;
    std::uint64_t bytesReturned_ = 0;
    std::uint64_t readCalls_ = 0;
    std::uint64_t seekCalls_ = 0;
    std::uint64_t maximumOffset_ = 0;
    std::vector<ReadRange> ranges_;
};

struct Box {
    std::uint32_t type = 0;
    std::uint64_t start = 0;
    std::uint64_t size = 0;
    std::uint64_t header = 0;
    std::uint64_t payload = 0;
    std::uint64_t end = 0;
};

Box parseBoxHeader(
    const std::uint8_t* data,
    std::uint64_t dataSize,
    std::uint64_t at,
    std::uint64_t parentEnd,
    bool allowZero) {
    if (!data || parentEnd > dataSize || at > parentEnd || parentEnd - at < 8) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidBox);
    }
    std::uint64_t size = be32(data + at);
    const std::uint32_t type = be32(data + at + 4);
    std::uint64_t header = 8;
    if (size == 1) {
        if (parentEnd - at < 16) {
            fail(Mp4Mp3SampleTableStatus::InvalidMedia,
                 Mp4Mp3SampleTableReason::InvalidBox);
        }
        size = be64(data + at + 8);
        header = 16;
    } else if (size == 0) {
        if (!allowZero) {
            fail(Mp4Mp3SampleTableStatus::UnsupportedLate,
                 Mp4Mp3SampleTableReason::UnsupportedExtendedNestedBox);
        }
        size = parentEnd - at;
    }
    if (size < header || size > parentEnd - at) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidBox);
    }
    return {type, at, size, header, at + header, at + size};
}

std::vector<Box> parseChildren(
    const std::vector<std::uint8_t>& data,
    std::uint64_t start,
    std::uint64_t end,
    std::uint64_t& boxesParsed) {
    std::vector<Box> boxes;
    std::uint64_t cursor = start;
    while (cursor < end) {
        if (++boxesParsed > kMaximumParsedBoxes) {
            fail(Mp4Mp3SampleTableStatus::UnsupportedLate,
                 Mp4Mp3SampleTableReason::ParserResourceLimit);
        }
        boxes.push_back(parseBoxHeader(data.data(), data.size(), cursor, end, false));
        cursor = boxes.back().end;
    }
    if (cursor != end) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidBox);
    }
    return boxes;
}

std::optional<Box> findOnly(
    const std::vector<Box>& boxes,
    std::uint32_t type,
    bool required,
    Mp4Mp3SampleTableReason missingReason = Mp4Mp3SampleTableReason::InvalidBox) {
    std::optional<Box> found;
    for (const Box& box : boxes) {
        if (box.type != type) {
            continue;
        }
        if (found) {
            fail(Mp4Mp3SampleTableStatus::Conflict,
                 Mp4Mp3SampleTableReason::InvalidBox);
        }
        found = box;
    }
    if (!found && required) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia, missingReason);
    }
    return found;
}

std::vector<Box> parseTopLevel(BoundedReader& reader) {
    std::vector<Box> boxes;
    boxes.reserve(8);
    std::uint64_t cursor = 0;
    while (cursor < reader.size()) {
        if (boxes.size() >= kMaximumTopLevelBoxes) {
            fail(Mp4Mp3SampleTableStatus::UnsupportedEarly,
                 Mp4Mp3SampleTableReason::ParserResourceLimit);
        }
        std::array<std::uint8_t, 16> header{};
        reader.readAt(cursor, header.data(), 8);
        std::uint64_t size = be32(header.data());
        const std::uint32_t type = be32(header.data() + 4);
        std::uint64_t headerSize = 8;
        if (size == 1) {
            reader.readAt(cursor + 8, header.data() + 8, 8);
            size = be64(header.data() + 8);
            headerSize = 16;
        } else if (size == 0) {
            size = reader.size() - cursor;
        }
        if (size < headerSize || size > reader.size() - cursor) {
            fail(Mp4Mp3SampleTableStatus::InvalidMedia,
                 Mp4Mp3SampleTableReason::InvalidBox);
        }
        boxes.push_back({type, cursor, size, headerSize,
                         cursor + headerSize, cursor + size});
        cursor += size;
    }
    return boxes;
}

struct MovieHeader {
    std::uint32_t timescale = 0;
    std::uint64_t duration = 0;
};

MovieHeader parseMvhd(const std::vector<std::uint8_t>& data, const Box& box) {
    const std::uint64_t length = box.end - box.payload;
    const std::uint8_t* value = data.data() + box.payload;
    if (length < 20) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidMovieHeader);
    }
    if (value[0] == 0) {
        return {be32(value + 12), be32(value + 16)};
    }
    if (value[0] == 1 && length >= 32) {
        return {be32(value + 20), be64(value + 24)};
    }
    fail(Mp4Mp3SampleTableStatus::UnsupportedLate,
         Mp4Mp3SampleTableReason::UnsupportedHeaderVersion);
}

struct TrackHeader {
    std::uint32_t id = 0;
    std::uint64_t duration = 0;
};

TrackHeader parseTkhd(const std::vector<std::uint8_t>& data, const Box& box) {
    const std::uint64_t length = box.end - box.payload;
    const std::uint8_t* value = data.data() + box.payload;
    if (length < 24) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidTrackHeader);
    }
    if (value[0] == 0) {
        return {be32(value + 12), be32(value + 20)};
    }
    if (value[0] == 1 && length >= 36) {
        return {be32(value + 20), be64(value + 28)};
    }
    fail(Mp4Mp3SampleTableStatus::UnsupportedLate,
         Mp4Mp3SampleTableReason::UnsupportedHeaderVersion);
}

MovieHeader parseMdhd(const std::vector<std::uint8_t>& data, const Box& box) {
    const std::uint64_t length = box.end - box.payload;
    const std::uint8_t* value = data.data() + box.payload;
    if (length < 20) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidMediaHeader);
    }
    if (value[0] == 0) {
        return {be32(value + 12), be32(value + 16)};
    }
    if (value[0] == 1 && length >= 32) {
        return {be32(value + 20), be64(value + 24)};
    }
    fail(Mp4Mp3SampleTableStatus::UnsupportedLate,
         Mp4Mp3SampleTableReason::UnsupportedHeaderVersion);
}

std::uint32_t parseHandler(const std::vector<std::uint8_t>& data, const Box& box) {
    if (box.end - box.payload < 12) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidHandler);
    }
    return be32(data.data() + box.payload + 8);
}

struct EditEntry {
    std::uint64_t duration = 0;
    std::int64_t mediaTime = -1;
    std::int16_t rateInteger = 0;
    std::int16_t rateFraction = 0;
};

EditEntry parseSingleEdit(
    const std::vector<std::uint8_t>& data,
    const Box& box,
    std::uint32_t& count) {
    const std::uint64_t length = box.end - box.payload;
    const std::uint8_t* value = data.data() + box.payload;
    if (length < 8) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidOrUnsupportedEditList);
    }
    const std::uint8_t version = value[0];
    count = be32(value + 4);
    const std::uint64_t width = version == 0 ? 12 : version == 1 ? 20 : 0;
    if (width == 0 || count != 1 || length != 8 + width) {
        fail(Mp4Mp3SampleTableStatus::UnsupportedLate,
             Mp4Mp3SampleTableReason::InvalidOrUnsupportedEditList);
    }
    EditEntry edit;
    if (version == 0) {
        edit.duration = be32(value + 8);
        edit.mediaTime = bes32(value + 12);
        edit.rateInteger = static_cast<std::int16_t>(be16(value + 16));
        edit.rateFraction = static_cast<std::int16_t>(be16(value + 18));
    } else {
        edit.duration = be64(value + 8);
        edit.mediaTime = bes64(value + 16);
        edit.rateInteger = static_cast<std::int16_t>(be16(value + 24));
        edit.rateFraction = static_cast<std::int16_t>(be16(value + 26));
    }
    if (edit.duration == 0 || edit.mediaTime < 0 ||
        edit.rateInteger != 1 || edit.rateFraction != 0) {
        fail(Mp4Mp3SampleTableStatus::UnsupportedLate,
             Mp4Mp3SampleTableReason::InvalidOrUnsupportedEditList);
    }
    return edit;
}

struct SttsEntry {
    std::uint32_t count = 0;
    std::uint32_t delta = 0;
};

std::vector<SttsEntry> parseStts(
    const std::vector<std::uint8_t>& data,
    const Box& box,
    std::uint64_t& sampleCount,
    std::uint64_t& duration,
    std::uint64_t& tableEntries) {
    const std::uint64_t length = box.end - box.payload;
    const std::uint8_t* value = data.data() + box.payload;
    if (length < 8) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidStts);
    }
    const std::uint32_t count = be32(value + 4);
    if (count == 0 || count > kMaximumSttsEntries || length != 8ULL + count * 8ULL) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidStts);
    }
    std::vector<SttsEntry> rows;
    rows.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint32_t samples = be32(value + 8 + index * 8ULL);
        const std::uint32_t delta = be32(value + 12 + index * 8ULL);
        std::uint64_t rowDuration = 0;
        if (samples == 0 || delta == 0 ||
            !checkedAdd(sampleCount, samples, sampleCount) ||
            !checkedMultiply(samples, delta, rowDuration) ||
            !checkedAdd(duration, rowDuration, duration)) {
            fail(Mp4Mp3SampleTableStatus::InvalidMedia,
                 Mp4Mp3SampleTableReason::InvalidStts);
        }
        rows.push_back({samples, delta});
    }
    tableEntries += count;
    return rows;
}

std::vector<std::uint32_t> parseStsz(
    const std::vector<std::uint8_t>& data,
    const Box& box,
    std::uint64_t& tableEntries) {
    const std::uint64_t length = box.end - box.payload;
    const std::uint8_t* value = data.data() + box.payload;
    if (length < 12) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidStsz);
    }
    const std::uint32_t fixed = be32(value + 4);
    const std::uint32_t count = be32(value + 8);
    if (count == 0 || count > kMaximumTableEntries ||
        (fixed != 0 ? length != 12 : length != 12ULL + count * 4ULL)) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidStsz);
    }
    std::vector<std::uint32_t> sizes;
    sizes.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint32_t size = fixed != 0 ? fixed : be32(value + 12 + index * 4ULL);
        if (size == 0) {
            fail(Mp4Mp3SampleTableStatus::InvalidMedia,
                 Mp4Mp3SampleTableReason::InvalidStsz);
        }
        sizes.push_back(size);
    }
    tableEntries += count;
    return sizes;
}

struct StscEntry {
    std::uint32_t firstChunk = 0;
    std::uint32_t samplesPerChunk = 0;
    std::uint32_t descriptionIndex = 0;
};

std::vector<StscEntry> parseStsc(
    const std::vector<std::uint8_t>& data,
    const Box& box,
    std::uint64_t& tableEntries) {
    const std::uint64_t length = box.end - box.payload;
    const std::uint8_t* value = data.data() + box.payload;
    if (length < 8) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidStsc);
    }
    const std::uint32_t count = be32(value + 4);
    if (count == 0 || count > kMaximumTableEntries || length != 8ULL + count * 12ULL) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidStsc);
    }
    std::vector<StscEntry> rows;
    rows.reserve(count);
    std::uint32_t previous = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint8_t* row = value + 8 + index * 12ULL;
        const StscEntry entry{be32(row), be32(row + 4), be32(row + 8)};
        if (entry.firstChunk <= previous || entry.samplesPerChunk == 0 ||
            entry.descriptionIndex == 0) {
            fail(Mp4Mp3SampleTableStatus::InvalidMedia,
                 Mp4Mp3SampleTableReason::InvalidStsc);
        }
        rows.push_back(entry);
        previous = entry.firstChunk;
    }
    if (rows.front().firstChunk != 1) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidStsc);
    }
    tableEntries += count;
    return rows;
}

std::vector<std::uint64_t> parseStco(
    const std::vector<std::uint8_t>& data,
    const Box& box,
    std::uint64_t& tableEntries) {
    const std::uint64_t length = box.end - box.payload;
    const std::uint8_t* value = data.data() + box.payload;
    if (length < 8) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidChunkOffsets);
    }
    const std::uint32_t count = be32(value + 4);
    if (count == 0 || count > kMaximumTableEntries || length != 8ULL + count * 4ULL) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidChunkOffsets);
    }
    std::vector<std::uint64_t> offsets;
    offsets.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        offsets.push_back(be32(value + 8 + index * 4ULL));
    }
    tableEntries += count;
    return offsets;
}

struct SampleEntry {
    std::uint32_t type = 0;
    std::uint16_t dataReferenceIndex = 0;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint8_t objectType = 0;
};

struct Descriptor {
    std::uint8_t tag = 0;
    std::size_t payload = 0;
    std::size_t end = 0;
};

Descriptor parseDescriptor(
    const std::uint8_t* data,
    std::size_t at,
    std::size_t limit) {
    if (!data || at >= limit) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidEsds);
    }
    Descriptor result;
    result.tag = data[at++];
    std::uint32_t size = 0;
    bool complete = false;
    for (int byteIndex = 0; byteIndex < 4 && at < limit; ++byteIndex) {
        const std::uint8_t byte = data[at++];
        size = (size << 7U) | (byte & 0x7FU);
        if ((byte & 0x80U) == 0) {
            complete = true;
            break;
        }
    }
    if (!complete || size > limit - at) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidEsds);
    }
    result.payload = at;
    result.end = at + size;
    return result;
}

std::uint8_t parseEsdsObjectType(
    const std::vector<std::uint8_t>& data,
    const Box& box) {
    if (box.end - box.payload < 6) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidEsds);
    }
    const std::uint8_t* value = data.data() + box.payload + 4;
    const std::size_t length = static_cast<std::size_t>(box.end - box.payload - 4);
    const Descriptor es = parseDescriptor(value, 0, length);
    if (es.tag != 0x03 || es.end != length || es.end - es.payload < 3) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidEsds);
    }
    std::size_t cursor = es.payload + 2;
    const std::uint8_t flags = value[cursor++];
    if ((flags & 0x80U) != 0) {
        if (es.end - cursor < 2) {
            fail(Mp4Mp3SampleTableStatus::InvalidMedia,
                 Mp4Mp3SampleTableReason::InvalidEsds);
        }
        cursor += 2;
    }
    if ((flags & 0x40U) != 0) {
        if (cursor >= es.end) {
            fail(Mp4Mp3SampleTableStatus::InvalidMedia,
                 Mp4Mp3SampleTableReason::InvalidEsds);
        }
        const std::size_t urlLength = value[cursor++];
        if (urlLength > es.end - cursor) {
            fail(Mp4Mp3SampleTableStatus::InvalidMedia,
                 Mp4Mp3SampleTableReason::InvalidEsds);
        }
        cursor += urlLength;
    }
    if ((flags & 0x20U) != 0) {
        if (es.end - cursor < 2) {
            fail(Mp4Mp3SampleTableStatus::InvalidMedia,
                 Mp4Mp3SampleTableReason::InvalidEsds);
        }
        cursor += 2;
    }
    const Descriptor decoderConfig = parseDescriptor(value, cursor, es.end);
    if (decoderConfig.tag != 0x04 || decoderConfig.end - decoderConfig.payload < 13) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidEsds);
    }
    return value[decoderConfig.payload];
}

SampleEntry parseStsd(
    const std::vector<std::uint8_t>& data,
    const Box& box,
    std::uint64_t& boxesParsed,
    std::uint64_t& tableEntries) {
    const std::uint64_t length = box.end - box.payload;
    const std::uint8_t* value = data.data() + box.payload;
    if (length < 8 || be32(value + 4) != 1) {
        fail(Mp4Mp3SampleTableStatus::UnsupportedLate,
             Mp4Mp3SampleTableReason::MultipleSampleDescriptions);
    }
    const Box entry = parseBoxHeader(data.data(), data.size(), box.payload + 8, box.end, false);
    if (entry.end != box.end || entry.end - entry.payload < 28) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::UnsupportedSampleEntry);
    }
    ++boxesParsed;
    if (entry.type == fourcc('e', 'n', 'c', 'a')) {
        fail(Mp4Mp3SampleTableStatus::UnsupportedEarly,
             Mp4Mp3SampleTableReason::ProtectedSampleEntry);
    }
    const std::uint8_t* body = data.data() + entry.payload;
    const std::uint16_t version = be16(body + 8);
    const std::uint64_t extra = version == 0 ? 0 : version == 1 ? 16 : version == 2 ? 36 : 0;
    if (version > 2 || entry.end - entry.payload < 28 + extra) {
        fail(Mp4Mp3SampleTableStatus::UnsupportedLate,
             Mp4Mp3SampleTableReason::UnsupportedSampleEntry);
    }
    SampleEntry result;
    result.type = entry.type;
    result.dataReferenceIndex = be16(body + 6);
    result.channels = be16(body + 16);
    result.sampleRate = be32(body + 24) >> 16U;
    const std::uint64_t childStart = entry.payload + 28 + extra;
    const std::vector<Box> children = parseChildren(data, childStart, entry.end, boxesParsed);
    const std::optional<Box> esds = findOnly(children, fourcc('e', 's', 'd', 's'), false);
    if (esds) {
        result.objectType = parseEsdsObjectType(data, *esds);
    }
    ++tableEntries;
    return result;
}

bool selectedDataReferenceSelfContained(
    const std::vector<std::uint8_t>& data,
    const Box& dref,
    std::uint16_t selectedIndex,
    std::uint64_t& boxesParsed,
    std::uint64_t& tableEntries) {
    const std::uint64_t length = dref.end - dref.payload;
    const std::uint8_t* value = data.data() + dref.payload;
    if (length < 8 || selectedIndex == 0) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidDataReference);
    }
    const std::uint32_t count = be32(value + 4);
    if (count == 0 || count > 64 || selectedIndex > count) {
        fail(Mp4Mp3SampleTableStatus::Conflict,
             Mp4Mp3SampleTableReason::InvalidDataReference);
    }
    std::uint64_t cursor = dref.payload + 8;
    bool selectedSelfContained = false;
    for (std::uint32_t index = 1; index <= count; ++index) {
        const Box entry = parseBoxHeader(data.data(), data.size(), cursor, dref.end, false);
        ++boxesParsed;
        if (entry.end - entry.payload < 4) {
            fail(Mp4Mp3SampleTableStatus::InvalidMedia,
                 Mp4Mp3SampleTableReason::InvalidDataReference);
        }
        if (index == selectedIndex) {
            selectedSelfContained = (data[entry.payload + 3] & 1U) != 0;
        }
        cursor = entry.end;
    }
    if (cursor != dref.end) {
        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
             Mp4Mp3SampleTableReason::InvalidDataReference);
    }
    tableEntries += count;
    return selectedSelfContained;
}

struct Mp3FrameHeader {
    int mpegVersion = 0;
    int sampleRate = 0;
    int bitrate = 0;
    int channels = 0;
    std::uint32_t samplesPerFrame = 0;
    std::uint32_t frameSize = 0;
};

Mp3FrameHeader parseMp3Header(const std::array<std::uint8_t, 4>& bytes) {
    const std::uint32_t word = be32(bytes.data());
    if ((word & 0xFFE00000U) != 0xFFE00000U) {
        fail(Mp4Mp3SampleTableStatus::Conflict,
             Mp4Mp3SampleTableReason::Mp3HeaderConflict);
    }
    const unsigned versionBits = (word >> 19U) & 3U;
    const unsigned layerBits = (word >> 17U) & 3U;
    const unsigned bitrateIndex = (word >> 12U) & 15U;
    const unsigned sampleRateIndex = (word >> 10U) & 3U;
    const unsigned padding = (word >> 9U) & 1U;
    const unsigned channelMode = (word >> 6U) & 3U;
    if (versionBits == 1 || layerBits != 1 || bitrateIndex == 0 ||
        bitrateIndex == 15 || sampleRateIndex == 3) {
        fail(Mp4Mp3SampleTableStatus::UnsupportedLate,
             Mp4Mp3SampleTableReason::UnsupportedMp3Profile);
    }
    constexpr std::array<int, 3> baseRates{44100, 48000, 32000};
    constexpr std::array<int, 15> v1Rates{
        0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    constexpr std::array<int, 15> v2Rates{
        0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    const int divisor = versionBits == 3 ? 1 : versionBits == 2 ? 2 : 4;
    Mp3FrameHeader result;
    result.mpegVersion = versionBits == 3 ? 1 : versionBits == 2 ? 2 : 25;
    result.sampleRate = baseRates[sampleRateIndex] / divisor;
    result.bitrate = (versionBits == 3 ? v1Rates[bitrateIndex] : v2Rates[bitrateIndex]) * 1000;
    result.channels = channelMode == 3 ? 1 : 2;
    result.samplesPerFrame = versionBits == 3 ? 1152 : 576;
    const std::uint32_t coefficient = versionBits == 3 ? 144U : 72U;
    result.frameSize = coefficient * static_cast<std::uint32_t>(result.bitrate) /
        static_cast<std::uint32_t>(result.sampleRate) + padding;
    return result;
}

bool rangeInsideMdat(
    std::uint64_t start,
    std::uint64_t end,
    const std::vector<ReadRange>& mdats) noexcept {
    return std::any_of(mdats.begin(), mdats.end(), [&](const ReadRange& range) {
        return start >= range.start && end <= range.end;
    });
}

bool formatListContains(const char* names, std::string_view expected) noexcept {
    if (!names) {
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

bool independentExact(const TotalPresentationEvidence& evidence) noexcept {
    return evidence.trust == PresentationTotalTrust::SampleExact &&
        evidence.domain == PresentationSampleDomain::NativeStreamSamples &&
        evidence.sampleRate > 0 && evidence.exactRescale && !evidence.conflict;
}

Mp4Mp3SampleEditTablePresentationResult finishResult(
    Mp4Mp3SampleEditTablePresentationResult result,
    const BoundedReader& reader,
    const std::chrono::steady_clock::time_point started) {
    result.bytesReturned = reader.bytesReturned();
    result.uniqueBytes = reader.uniqueBytes();
    result.duplicateBytes = result.bytesReturned - result.uniqueBytes;
    result.maximumBudgetOverrunBytes = result.bytesReturned > result.hardReadBudgetBytes
        ? result.bytesReturned - result.hardReadBudgetBytes
        : 0;
    result.readCalls = reader.readCalls();
    result.seekCalls = reader.seekCalls();
    result.maximumOffsetReached = reader.maximumOffsetReached();
    result.scanDurationUs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count());
    return result;
}

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
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor++] != ':') {
        return false;
    }
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
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
        cursor >= text.size() || !std::isdigit(static_cast<unsigned char>(text[cursor]))) {
        return false;
    }
    std::uint64_t parsed = 0;
    while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor]))) {
        const std::uint64_t digit = static_cast<unsigned>(text[cursor++] - '0');
        if (parsed > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    value = parsed;
    return true;
}

bool parseJsonBool(const std::string& text, std::string_view key, bool& value) {
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

bool readProbeDocument(const std::filesystem::path& path, std::string& text) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > kMaximumProbeDocumentBytes) {
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    text.resize(static_cast<std::size_t>(size));
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    return input && input.gcount() == static_cast<std::streamsize>(text.size());
}

}  // namespace

Mp4Mp3SampleTableEligibility evaluateMp4Mp3SampleTableEligibility(
    const std::string& path,
    const AVFormatContext* formatContext,
    const AVStream* selectedAudioStream,
    bool strongerSampleExactAuthorityPresent) noexcept {
    Mp4Mp3SampleTableEligibility result;
    try {
        if (strongerSampleExactAuthorityPresent || path.empty()) {
            return result;
        }
        if (!formatContext || !formatContext->iformat ||
            !formatListContains(formatContext->iformat->name, "mov")) {
            result.reason = Mp4Mp3SampleTableReason::NotMovMp4;
            return result;
        }
        if (!selectedAudioStream || !selectedAudioStream->codecpar) {
            result.reason = Mp4Mp3SampleTableReason::SelectedStreamAbsent;
            return result;
        }
        const AVCodecParameters* codecpar = selectedAudioStream->codecpar;
        if (codecpar->codec_type != AVMEDIA_TYPE_AUDIO ||
            codecpar->codec_id != AV_CODEC_ID_MP3) {
            result.reason = Mp4Mp3SampleTableReason::SelectedCodecNotMp3;
            return result;
        }
        if (selectedAudioStream->id <= 0 ||
            static_cast<std::uint64_t>(selectedAudioStream->id) >
                (std::numeric_limits<std::uint32_t>::max)()) {
            result.reason = Mp4Mp3SampleTableReason::SelectedTrackIdInvalid;
            return result;
        }
        FileIdentity identity;
        if (!readFileIdentity(path, identity)) {
            result.reason = Mp4Mp3SampleTableReason::InputNotRegularFile;
            return result;
        }
        if (codecpar->sample_rate <= 0 || codecpar->ch_layout.nb_channels <= 0) {
            result.reason = Mp4Mp3SampleTableReason::SelectedStreamAbsent;
            return result;
        }
        result.eligible = true;
        result.reason = Mp4Mp3SampleTableReason::ValidatedClassicSampleEditTables;
        result.selectedStreamIndex = static_cast<int>(selectedAudioStream->index);
        result.selectedTrackId = static_cast<std::uint32_t>(selectedAudioStream->id);
        result.sampleRate = codecpar->sample_rate;
        result.channels = codecpar->ch_layout.nb_channels;
        return result;
    } catch (...) {
        result = {};
        return result;
    }
}

Mp4Mp3SampleEditTablePresentationResult
probeMp4Mp3SampleEditTablePresentation(
    const std::string& path,
    const Mp4Mp3SampleTableEligibility& eligibility,
    const Mp4Mp3SampleTableProbeOptions& options) {
    const auto started = std::chrono::steady_clock::now();
    Mp4Mp3SampleEditTablePresentationResult result;
    result.hardReadBudgetBytes = options.hardReadBudgetBytes;
    result.selectedStreamIndex = eligibility.selectedStreamIndex;
    result.selectedTrackId = eligibility.selectedTrackId;
    result.sampleRate = eligibility.sampleRate;
    result.channels = eligibility.channels;
    if (!eligibility.eligible || options.hardReadBudgetBytes == 0) {
        result.reason = eligibility.reason;
        return result;
    }

    try {
        BoundedReader reader(path, options);
        result.fileSizeBytes = reader.identity().size;
        result.fileIndex = reader.identity().index;
        result.lastWriteTime100ns = reader.identity().lastWriteTime;
        result.volumeSerialNumber = reader.identity().volume;

        try {
            const std::vector<Box> top = parseTopLevel(reader);
            std::optional<Box> moov;
            std::vector<ReadRange> mdats;
            mdats.reserve(4);
            for (const Box& box : top) {
                if (box.type == fourcc('m', 'o', 'o', 'f') ||
                    box.type == fourcc('s', 'i', 'd', 'x') ||
                    box.type == fourcc('m', 'f', 'r', 'a')) {
                    fail(Mp4Mp3SampleTableStatus::UnsupportedEarly,
                         Mp4Mp3SampleTableReason::FragmentedMp4);
                }
                if (box.type == fourcc('m', 'o', 'o', 'v')) {
                    if (moov) {
                        fail(Mp4Mp3SampleTableStatus::Conflict,
                             Mp4Mp3SampleTableReason::DuplicateMoov);
                    }
                    moov = box;
                } else if (box.type == fourcc('m', 'd', 'a', 't')) {
                    mdats.push_back({box.payload, box.end});
                }
            }
            if (!moov) {
                fail(Mp4Mp3SampleTableStatus::UnsupportedEarly,
                     Mp4Mp3SampleTableReason::MissingMoov);
            }
            if (mdats.empty()) {
                fail(Mp4Mp3SampleTableStatus::InvalidMedia,
                     Mp4Mp3SampleTableReason::MissingMdat);
            }
            result.moovOffset = moov->start;
            result.moovSize = moov->size;
            result.moovAtHead = moov->start < mdats.front().start;
            result.moovAtTail = moov->end == reader.size() ||
                moov->start >= mdats.back().end;
            if (moov->size > options.hardReadBudgetBytes -
                    std::min(options.hardReadBudgetBytes, reader.bytesReturned())) {
                fail(Mp4Mp3SampleTableStatus::IoBudgetExceeded,
                     Mp4Mp3SampleTableReason::IoBudgetExceeded);
            }

            const std::vector<std::uint8_t> data = reader.readAt(moov->start, moov->size);
            result.maximumWorkingBufferBytes = data.capacity();
            const Box root = parseBoxHeader(data.data(), data.size(), 0, data.size(), false);
            if (root.type != fourcc('m', 'o', 'o', 'v') || root.end != data.size()) {
                fail(Mp4Mp3SampleTableStatus::InvalidMedia,
                     Mp4Mp3SampleTableReason::InvalidBox);
            }
            result.reachedRequiredMoovEnd = true;
            result.boxesParsed = 1;
            const std::vector<Box> moovChildren =
                parseChildren(data, root.payload, root.end, result.boxesParsed);
            const Box mvhd = *findOnly(
                moovChildren, fourcc('m', 'v', 'h', 'd'), true,
                Mp4Mp3SampleTableReason::InvalidMovieHeader);
            const MovieHeader movie = parseMvhd(data, mvhd);
            if (movie.timescale == 0) {
                fail(Mp4Mp3SampleTableStatus::InvalidMedia,
                     Mp4Mp3SampleTableReason::InvalidMovieHeader);
            }
            result.movieTimescale = movie.timescale;

            std::optional<Box> selectedTrak;
            TrackHeader selectedTkhd;
            std::size_t tracks = 0;
            for (const Box& trak : moovChildren) {
                if (trak.type != fourcc('t', 'r', 'a', 'k')) {
                    continue;
                }
                if (++tracks > kMaximumTracks) {
                    fail(Mp4Mp3SampleTableStatus::UnsupportedLate,
                         Mp4Mp3SampleTableReason::ParserResourceLimit);
                }
                const std::vector<Box> trackChildren =
                    parseChildren(data, trak.payload, trak.end, result.boxesParsed);
                const Box tkhd = *findOnly(
                    trackChildren, fourcc('t', 'k', 'h', 'd'), true,
                    Mp4Mp3SampleTableReason::InvalidTrackHeader);
                const TrackHeader parsed = parseTkhd(data, tkhd);
                if (parsed.id != eligibility.selectedTrackId) {
                    continue;
                }
                if (selectedTrak) {
                    fail(Mp4Mp3SampleTableStatus::Conflict,
                         Mp4Mp3SampleTableReason::DuplicateSelectedTrack);
                }
                selectedTrak = trak;
                selectedTkhd = parsed;
            }
            if (!selectedTrak) {
                fail(Mp4Mp3SampleTableStatus::Conflict,
                     Mp4Mp3SampleTableReason::SelectedTrackAbsent);
            }

            const std::vector<Box> trackChildren =
                parseChildren(data, selectedTrak->payload, selectedTrak->end, result.boxesParsed);
            const Box mdia = *findOnly(trackChildren, fourcc('m', 'd', 'i', 'a'), true);
            const Box edts = *findOnly(trackChildren, fourcc('e', 'd', 't', 's'), true);
            const std::vector<Box> mediaChildren =
                parseChildren(data, mdia.payload, mdia.end, result.boxesParsed);
            const MovieHeader media = parseMdhd(
                data, *findOnly(mediaChildren, fourcc('m', 'd', 'h', 'd'), true,
                               Mp4Mp3SampleTableReason::InvalidMediaHeader));
            if (media.timescale == 0) {
                fail(Mp4Mp3SampleTableStatus::InvalidMedia,
                     Mp4Mp3SampleTableReason::InvalidMediaHeader);
            }
            result.mediaTimescale = media.timescale;
            if (parseHandler(
                    data, *findOnly(mediaChildren, fourcc('h', 'd', 'l', 'r'), true,
                                   Mp4Mp3SampleTableReason::InvalidHandler)) !=
                fourcc('s', 'o', 'u', 'n')) {
                fail(Mp4Mp3SampleTableStatus::Conflict,
                     Mp4Mp3SampleTableReason::SelectedTrackNotAudio);
            }

            const std::vector<Box> editChildren =
                parseChildren(data, edts.payload, edts.end, result.boxesParsed);
            const EditEntry edit = parseSingleEdit(
                data, *findOnly(editChildren, fourcc('e', 'l', 's', 't'), true),
                result.editCount);
            result.editMediaStart = static_cast<std::uint64_t>(edit.mediaTime);
            if (edit.duration != selectedTkhd.duration) {
                fail(Mp4Mp3SampleTableStatus::Conflict,
                     Mp4Mp3SampleTableReason::EditTimelineConflict);
            }
            std::uint64_t scaledDuration = 0;
            if (!checkedMultiply(edit.duration, media.timescale, scaledDuration)) {
                fail(Mp4Mp3SampleTableStatus::InvalidMedia,
                     Mp4Mp3SampleTableReason::EditDurationNotSampleExact);
            }
            if (scaledDuration % movie.timescale != 0) {
                fail(Mp4Mp3SampleTableStatus::UnsupportedLate,
                     Mp4Mp3SampleTableReason::EditDurationNotSampleExact);
            }
            result.editPresentationFrames = scaledDuration / movie.timescale;
            if (result.editPresentationFrames == 0 ||
                !checkedAdd(result.editMediaStart, result.editPresentationFrames,
                            result.editedMediaEnd)) {
                fail(Mp4Mp3SampleTableStatus::InvalidMedia,
                     Mp4Mp3SampleTableReason::EditTimelineConflict);
            }
            result.editListValid = true;

            const Box minf = *findOnly(mediaChildren, fourcc('m', 'i', 'n', 'f'), true);
            const std::vector<Box> minfChildren =
                parseChildren(data, minf.payload, minf.end, result.boxesParsed);
            const Box dinf = *findOnly(minfChildren, fourcc('d', 'i', 'n', 'f'), true);
            const Box stbl = *findOnly(minfChildren, fourcc('s', 't', 'b', 'l'), true);
            const std::vector<Box> stblChildren =
                parseChildren(data, stbl.payload, stbl.end, result.boxesParsed);
            if (findOnly(stblChildren, fourcc('c', 't', 't', 's'), false)) {
                fail(Mp4Mp3SampleTableStatus::UnsupportedLate,
                     Mp4Mp3SampleTableReason::UnsupportedSampleSizeTable);
            }

            const SampleEntry sampleEntry = parseStsd(
                data, *findOnly(stblChildren, fourcc('s', 't', 's', 'd'), true),
                result.boxesParsed, result.tableEntriesParsed);
            result.sampleEntry = sampleEntry.type;
            result.objectTypeIndication = sampleEntry.objectType;
            if (sampleEntry.type == fourcc('e', 'n', 'c', 'a')) {
                fail(Mp4Mp3SampleTableStatus::UnsupportedEarly,
                     Mp4Mp3SampleTableReason::ProtectedSampleEntry);
            }
            const bool mp3Entry = sampleEntry.type == fourcc('.', 'm', 'p', '3') ||
                sampleEntry.type == fourcc('m', 'p', '3', ' ') ||
                (sampleEntry.type == fourcc('m', 'p', '4', 'a') &&
                 (sampleEntry.objectType == 0x69 || sampleEntry.objectType == 0x6B));
            if (!mp3Entry) {
                fail(Mp4Mp3SampleTableStatus::UnsupportedEarly,
                     sampleEntry.type == fourcc('m', 'p', '4', 'a')
                         ? Mp4Mp3SampleTableReason::UnsupportedMp3ObjectType
                         : Mp4Mp3SampleTableReason::UnsupportedSampleEntry);
            }
            if (sampleEntry.sampleRate != media.timescale ||
                sampleEntry.sampleRate != static_cast<std::uint32_t>(eligibility.sampleRate) ||
                sampleEntry.channels != eligibility.channels) {
                fail(Mp4Mp3SampleTableStatus::Conflict,
                     Mp4Mp3SampleTableReason::SampleDescriptionConflict);
            }

            const std::vector<Box> dinfChildren =
                parseChildren(data, dinf.payload, dinf.end, result.boxesParsed);
            const Box dref = *findOnly(dinfChildren, fourcc('d', 'r', 'e', 'f'), true);
            if (!selectedDataReferenceSelfContained(
                    data, dref, sampleEntry.dataReferenceIndex,
                    result.boxesParsed, result.tableEntriesParsed)) {
                fail(Mp4Mp3SampleTableStatus::UnsupportedEarly,
                     Mp4Mp3SampleTableReason::ExternalDataReference);
            }

            std::uint64_t sttsCount = 0;
            std::uint64_t sttsDuration = 0;
            const std::vector<SttsEntry> stts = parseStts(
                data, *findOnly(stblChildren, fourcc('s', 't', 't', 's'), true),
                sttsCount, sttsDuration, result.tableEntriesParsed);
            const std::optional<Box> stsz = findOnly(
                stblChildren, fourcc('s', 't', 's', 'z'), false);
            const std::optional<Box> stz2 = findOnly(
                stblChildren, fourcc('s', 't', 'z', '2'), false);
            if (!stsz || stz2) {
                fail(Mp4Mp3SampleTableStatus::UnsupportedEarly,
                     Mp4Mp3SampleTableReason::UnsupportedSampleSizeTable);
            }
            const std::vector<std::uint32_t> sizes =
                parseStsz(data, *stsz, result.tableEntriesParsed);
            const std::vector<StscEntry> stsc = parseStsc(
                data, *findOnly(stblChildren, fourcc('s', 't', 's', 'c'), true),
                result.tableEntriesParsed);
            const std::optional<Box> stco = findOnly(
                stblChildren, fourcc('s', 't', 'c', 'o'), false);
            const std::optional<Box> co64 = findOnly(
                stblChildren, fourcc('c', 'o', '6', '4'), false);
            if (!stco || co64) {
                fail(Mp4Mp3SampleTableStatus::UnsupportedEarly,
                     Mp4Mp3SampleTableReason::UnsupportedChunkOffsetTable);
            }
            const std::vector<std::uint64_t> offsets =
                parseStco(data, *stco, result.tableEntriesParsed);
            result.maximumWorkingBufferBytes +=
                sizes.capacity() * sizeof(std::uint32_t) +
                stsc.capacity() * sizeof(StscEntry) +
                offsets.capacity() * sizeof(std::uint64_t) +
                stts.capacity() * sizeof(SttsEntry);

            if (sttsCount != sizes.size()) {
                fail(Mp4Mp3SampleTableStatus::Conflict,
                     Mp4Mp3SampleTableReason::SampleCountConflict);
            }
            if (media.duration != sttsDuration || result.editedMediaEnd != sttsDuration) {
                fail(Mp4Mp3SampleTableStatus::Conflict,
                     Mp4Mp3SampleTableReason::DurationConflict);
            }
            result.selectedSampleCount = sttsCount;
            result.sampleInventoryValid = true;

            std::size_t sampleIndex = 0;
            std::size_t stscIndex = 0;
            std::uint64_t firstOffset = 0;
            std::uint64_t lastOffset = 0;
            std::uint64_t previousChunkEnd = 0;
            for (std::size_t chunkIndex = 0; chunkIndex < offsets.size(); ++chunkIndex) {
                const std::uint32_t chunkNumber = static_cast<std::uint32_t>(chunkIndex + 1);
                while (stscIndex + 1 < stsc.size() &&
                       chunkNumber >= stsc[stscIndex + 1].firstChunk) {
                    ++stscIndex;
                }
                const StscEntry& mapping = stsc[stscIndex];
                if (mapping.descriptionIndex != 1) {
                    fail(Mp4Mp3SampleTableStatus::Conflict,
                         Mp4Mp3SampleTableReason::SampleDescriptionConflict);
                }
                if (mapping.samplesPerChunk > sizes.size() - sampleIndex) {
                    fail(Mp4Mp3SampleTableStatus::Conflict,
                         Mp4Mp3SampleTableReason::ChunkMappingConflict);
                }
                std::uint64_t chunkBytes = 0;
                std::uint64_t localOffset = 0;
                for (std::uint32_t inChunk = 0; inChunk < mapping.samplesPerChunk; ++inChunk) {
                    if (sampleIndex + inChunk == sizes.size() - 1) {
                        if (!checkedAdd(offsets[chunkIndex], localOffset, lastOffset)) {
                            fail(Mp4Mp3SampleTableStatus::InvalidMedia,
                                 Mp4Mp3SampleTableReason::ChunkMappingConflict);
                        }
                    }
                    if (!checkedAdd(localOffset, sizes[sampleIndex + inChunk], localOffset) ||
                        !checkedAdd(chunkBytes, sizes[sampleIndex + inChunk], chunkBytes)) {
                        fail(Mp4Mp3SampleTableStatus::InvalidMedia,
                             Mp4Mp3SampleTableReason::ChunkMappingConflict);
                    }
                }
                std::uint64_t chunkEnd = 0;
                if (!checkedAdd(offsets[chunkIndex], chunkBytes, chunkEnd)) {
                    fail(Mp4Mp3SampleTableStatus::InvalidMedia,
                         Mp4Mp3SampleTableReason::ChunkMappingConflict);
                }
                if (!rangeInsideMdat(offsets[chunkIndex], chunkEnd, mdats)) {
                    fail(Mp4Mp3SampleTableStatus::Conflict,
                         Mp4Mp3SampleTableReason::ChunkOutsideMdat);
                }
                if (chunkIndex != 0 && offsets[chunkIndex] < previousChunkEnd) {
                    fail(Mp4Mp3SampleTableStatus::Conflict,
                         Mp4Mp3SampleTableReason::ChunkOverlap);
                }
                if (chunkIndex == 0) {
                    firstOffset = offsets[chunkIndex];
                }
                previousChunkEnd = chunkEnd;
                sampleIndex += mapping.samplesPerChunk;
                ++result.selectedChunks;
            }
            if (sampleIndex != sizes.size() || firstOffset == 0 || lastOffset == 0) {
                fail(Mp4Mp3SampleTableStatus::Conflict,
                     Mp4Mp3SampleTableReason::ChunkMappingConflict);
            }
            result.chunkMappingValid = true;
            result.chunkRangesInsideMdat = true;

            std::array<std::uint8_t, 4> firstBytes{};
            std::array<std::uint8_t, 4> lastBytes{};
            reader.readAt(firstOffset, firstBytes.data(), firstBytes.size());
            reader.readAt(lastOffset, lastBytes.data(), lastBytes.size());
            const Mp3FrameHeader first = parseMp3Header(firstBytes);
            const Mp3FrameHeader last = parseMp3Header(lastBytes);
            if (first.mpegVersion != last.mpegVersion ||
                first.sampleRate != last.sampleRate ||
                first.bitrate != last.bitrate || first.channels != last.channels) {
                fail(Mp4Mp3SampleTableStatus::UnsupportedLate,
                     Mp4Mp3SampleTableReason::VbrOrProfileChange);
            }
            if (first.sampleRate != eligibility.sampleRate ||
                first.channels != eligibility.channels) {
                fail(Mp4Mp3SampleTableStatus::Conflict,
                     Mp4Mp3SampleTableReason::Mp3HeaderConflict);
            }
            const std::uint32_t coefficient = first.mpegVersion == 1 ? 144U : 72U;
            const std::uint32_t baseSize = coefficient *
                static_cast<std::uint32_t>(first.bitrate) /
                static_cast<std::uint32_t>(first.sampleRate);
            if (sizes.front() != first.frameSize || sizes.back() != last.frameSize ||
                std::any_of(sizes.begin(), sizes.end(), [&](std::uint32_t size) {
                    return size != baseSize && size != baseSize + 1;
                })) {
                fail(Mp4Mp3SampleTableStatus::UnsupportedLate,
                     Mp4Mp3SampleTableReason::SampleSizeNotSingleCbrFrame);
            }
            for (std::size_t index = 0; index + 1 < stts.size(); ++index) {
                if (stts[index].delta != first.samplesPerFrame) {
                    fail(Mp4Mp3SampleTableStatus::UnsupportedLate,
                         Mp4Mp3SampleTableReason::UnsupportedMp3Profile);
                }
            }
            if (stts.back().delta > first.samplesPerFrame ||
                (stts.back().delta != first.samplesPerFrame && stts.back().count != 1)) {
                fail(Mp4Mp3SampleTableStatus::Conflict,
                     Mp4Mp3SampleTableReason::TerminalDiscardConflict);
            }
            result.mpegVersion = first.mpegVersion;
            result.samplesPerMp3Frame = first.samplesPerFrame;
            result.mp3FramesPerMp4Sample = 1;
            if (!checkedMultiply(sttsCount, first.samplesPerFrame, result.physicalFrames) ||
                result.physicalFrames < sttsDuration) {
                fail(Mp4Mp3SampleTableStatus::Conflict,
                     Mp4Mp3SampleTableReason::TerminalDiscardConflict);
            }
            result.terminalDiscardFrames = result.physicalFrames - sttsDuration;
            if (result.terminalDiscardFrames >= first.samplesPerFrame ||
                result.physicalFrames < result.editMediaStart ||
                result.physicalFrames - result.editMediaStart < result.terminalDiscardFrames ||
                result.physicalFrames - result.editMediaStart -
                    result.terminalDiscardFrames != result.editPresentationFrames) {
                fail(Mp4Mp3SampleTableStatus::Conflict,
                     Mp4Mp3SampleTableReason::TerminalDiscardConflict);
            }
            result.initialSkipFrames = result.editMediaStart;
            result.presentationFrames = result.editPresentationFrames;
            result.mp3ProfileValid = true;
            result.checkedArithmeticValid = true;

            if (!reader.unchanged()) {
                fail(Mp4Mp3SampleTableStatus::Conflict,
                     Mp4Mp3SampleTableReason::InputChangedDuringProbe);
            }
            result.status = Mp4Mp3SampleTableStatus::Exact;
            result.reason = Mp4Mp3SampleTableReason::ValidatedClassicSampleEditTables;
        } catch (const Failure& error) {
            result.status = error.status;
            result.reason = error.reason;
        }
        return finishResult(std::move(result), reader, started);
    } catch (const Failure& error) {
        result.status = error.status;
        result.reason = error.reason;
        result.scanDurationUs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count());
        return result;
    } catch (...) {
        result.status = Mp4Mp3SampleTableStatus::IoError;
        result.reason = Mp4Mp3SampleTableReason::InputReadFailed;
        result.scanDurationUs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count());
        return result;
    }
}

bool mp4Mp3SampleTableMatchesStream(
    const Mp4Mp3SampleEditTablePresentationResult& result,
    const AVStream* selectedAudioStream) noexcept {
    if (!result.exact() || !selectedAudioStream || !selectedAudioStream->codecpar) {
        return false;
    }
    const AVCodecParameters* codecpar = selectedAudioStream->codecpar;
    if (codecpar->codec_id != AV_CODEC_ID_MP3 ||
        static_cast<int>(selectedAudioStream->index) != result.selectedStreamIndex ||
        selectedAudioStream->id <= 0 ||
        static_cast<std::uint32_t>(selectedAudioStream->id) != result.selectedTrackId ||
        codecpar->sample_rate != result.sampleRate ||
        codecpar->ch_layout.nb_channels != result.channels ||
        (codecpar->frame_size > 0 &&
         static_cast<std::uint64_t>(codecpar->frame_size) != result.samplesPerMp3Frame) ||
        (selectedAudioStream->nb_frames > 0 &&
         static_cast<std::uint64_t>(selectedAudioStream->nb_frames) !=
             result.selectedSampleCount)) {
        return false;
    }
    if (selectedAudioStream->duration != AV_NOPTS_VALUE &&
        selectedAudioStream->duration > 0) {
        const AVRational native{1, result.sampleRate};
        const std::int64_t frames = av_rescale_q(
            selectedAudioStream->duration, selectedAudioStream->time_base, native);
        if (frames < 0 || static_cast<std::uint64_t>(frames) != result.presentationFrames ||
            av_compare_ts(selectedAudioStream->duration, selectedAudioStream->time_base,
                          frames, native) != 0) {
            return false;
        }
    }
    return true;
}

TotalPresentationEvidence makeMp4Mp3SampleTableTotalPresentationEvidence(
    const Mp4Mp3SampleEditTablePresentationResult& result) noexcept {
    TotalPresentationEvidence evidence;
    if (!result.exact() || result.presentationFrames == 0 || result.sampleRate <= 0 ||
        !result.sampleInventoryValid || !result.chunkMappingValid ||
        !result.chunkRangesInsideMdat || !result.editListValid ||
        !result.mp3ProfileValid || !result.checkedArithmeticValid ||
        result.maximumBudgetOverrunBytes != 0) {
        return evidence;
    }
    evidence.frames = result.presentationFrames;
    evidence.trust = PresentationTotalTrust::SampleExact;
    evidence.source = PresentationTotalSource::Mp4Mp3SampleEditTablePresentation;
    evidence.domain = PresentationSampleDomain::NativeStreamSamples;
    evidence.sampleRate = result.sampleRate;
    evidence.exactRescale = true;
    evidence.validation = PresentationTotalValidation::SelfContainedMetadata;
    return evidence;
}

Mp4Mp3SampleTableHandoffResult readMp4Mp3SampleTablePresentationHandoff(
    const std::filesystem::path& probeJsonPath,
    const std::string& sourcePath,
    const AVFormatContext* formatContext,
    const AVStream* selectedAudioStream,
    const TotalPresentationEvidence& independentEvidence) noexcept {
    Mp4Mp3SampleTableHandoffResult handoff;
    try {
        const Mp4Mp3SampleTableEligibility eligibility =
            evaluateMp4Mp3SampleTableEligibility(
                sourcePath, formatContext, selectedAudioStream, false);
        if (!eligibility.eligible) {
            handoff.reason = eligibility.reason;
            return handoff;
        }
        std::string document;
        if (!readProbeDocument(probeJsonPath, document)) {
            return handoff;
        }
        std::string recordedPath;
        std::string recordedStatus;
        std::string recordedReason;
        std::string recordedSource;
        bool genericScanSkipped = false;
        auto& result = handoff.presentation;
        std::uint64_t selectedStream = 0;
        std::uint64_t selectedTrack = 0;
        std::uint64_t sampleRate = 0;
        std::uint64_t channels = 0;
        std::uint64_t movieTimescale = 0;
        std::uint64_t mediaTimescale = 0;
        std::uint64_t volume = 0;
        bool valid =
            parseJsonString(document, "sourcePath", recordedPath) &&
            parseJsonString(document, "mp4Mp3SampleTableStatus", recordedStatus) &&
            parseJsonString(document, "mp4Mp3SampleTableReason", recordedReason) &&
            parseJsonString(document, "decodedSampleFramesSource", recordedSource) &&
            parseJsonUnsigned(document, "selectedAudioStreamIndex", selectedStream) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTableSelectedTrackId", selectedTrack) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTableSampleRate", sampleRate) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTableChannels", channels) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTableSelectedSamples", result.selectedSampleCount) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTableSamplesPerMp3Frame", result.samplesPerMp3Frame) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTablePhysicalFrames", result.physicalFrames) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTableInitialSkip", result.initialSkipFrames) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTableTerminalDiscard", result.terminalDiscardFrames) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTableEditedMediaEnd", result.editedMediaEnd) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTablePresentationFrames", result.presentationFrames) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTableMovieTimescale", movieTimescale) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTableMediaTimescale", mediaTimescale) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTableEditMediaStart", result.editMediaStart) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTableEditPresentationFrames", result.editPresentationFrames) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTableFileSizeBytes", result.fileSizeBytes) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTableFileIndex", result.fileIndex) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTableLastWriteTime100ns", result.lastWriteTime100ns) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTableVolumeSerialNumber", volume) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTableBytesReturned", result.bytesReturned) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTableUniqueBytes", result.uniqueBytes) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTableReadCalls", result.readCalls) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTableSeekCalls", result.seekCalls) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTableMoovOffset", result.moovOffset) &&
            parseJsonUnsigned(document, "mp4Mp3SampleTableMoovSize", result.moovSize) &&
            parseJsonBool(document, "mp4Mp3SampleTableSampleInventoryValid", result.sampleInventoryValid) &&
            parseJsonBool(document, "mp4Mp3SampleTableChunkMappingValid", result.chunkMappingValid) &&
            parseJsonBool(document, "mp4Mp3SampleTableChunkRangesInsideMdat", result.chunkRangesInsideMdat) &&
            parseJsonBool(document, "mp4Mp3SampleTableEditListValid", result.editListValid) &&
            parseJsonBool(document, "mp4Mp3SampleTableMp3ProfileValid", result.mp3ProfileValid) &&
            parseJsonBool(document, "mp4Mp3SampleTableCheckedArithmeticValid", result.checkedArithmeticValid) &&
            parseJsonBool(document, "mp4Mp3SampleTableGenericScanSkipped", genericScanSkipped);
        if (!valid || recordedPath != sourcePath || recordedStatus != "exact" ||
            recordedReason != "validated_classic_sample_edit_tables" ||
            recordedSource != "mp4_mp3_sample_edit_table_presentation" ||
            selectedStream != static_cast<std::uint64_t>(eligibility.selectedStreamIndex) ||
            selectedTrack != eligibility.selectedTrackId || sampleRate != eligibility.sampleRate ||
            channels != eligibility.channels ||
            movieTimescale > (std::numeric_limits<std::uint32_t>::max)() ||
            mediaTimescale > (std::numeric_limits<std::uint32_t>::max)() ||
            volume > (std::numeric_limits<std::uint32_t>::max)() ||
            !genericScanSkipped) {
            handoff.reason = Mp4Mp3SampleTableReason::ProbeDocumentInvalid;
            return handoff;
        }
        result.selectedStreamIndex = eligibility.selectedStreamIndex;
        result.selectedTrackId = eligibility.selectedTrackId;
        result.sampleRate = eligibility.sampleRate;
        result.channels = eligibility.channels;
        result.movieTimescale = static_cast<std::uint32_t>(movieTimescale);
        result.mediaTimescale = static_cast<std::uint32_t>(mediaTimescale);
        result.volumeSerialNumber = static_cast<std::uint32_t>(volume);
        result.mp3FramesPerMp4Sample = 1;
        std::uint64_t checkedEditedMediaEnd = 0;
        result.editListValid = result.editListValid &&
            result.editMediaStart == result.initialSkipFrames &&
            result.editPresentationFrames == result.presentationFrames &&
            checkedAdd(result.editMediaStart, result.editPresentationFrames,
                       checkedEditedMediaEnd) &&
            result.editedMediaEnd == checkedEditedMediaEnd;
        result.mp3ProfileValid = result.mp3ProfileValid && result.samplesPerMp3Frame > 0;
        result.checkedArithmeticValid = result.checkedArithmeticValid &&
            result.physicalFrames >= result.initialSkipFrames &&
            result.physicalFrames - result.initialSkipFrames >= result.terminalDiscardFrames &&
            result.physicalFrames - result.initialSkipFrames -
                result.terminalDiscardFrames == result.presentationFrames;
        result.maximumBudgetOverrunBytes = result.bytesReturned > kMp4Mp3SampleTableReadBudgetBytes
            ? result.bytesReturned - kMp4Mp3SampleTableReadBudgetBytes
            : 0;
        FileIdentity current;
        if (!readFileIdentity(sourcePath, current) || current.size != result.fileSizeBytes ||
            current.index != result.fileIndex || current.lastWriteTime != result.lastWriteTime100ns ||
            current.volume != result.volumeSerialNumber) {
            handoff.status = Mp4Mp3SampleTableHandoffStatus::Conflict;
            handoff.reason = Mp4Mp3SampleTableReason::SourceIdentityMismatch;
            return handoff;
        }
        result.status = Mp4Mp3SampleTableStatus::Exact;
        result.reason = Mp4Mp3SampleTableReason::ValidatedClassicSampleEditTables;
        if (!mp4Mp3SampleTableMatchesStream(result, selectedAudioStream)) {
            handoff.status = Mp4Mp3SampleTableHandoffStatus::Conflict;
            handoff.reason = Mp4Mp3SampleTableReason::IndependentExactAuthorityConflict;
            return handoff;
        }
        handoff.evidence = makeMp4Mp3SampleTableTotalPresentationEvidence(result);
        if (independentExact(independentEvidence) &&
            (independentEvidence.frames != handoff.evidence.frames ||
             independentEvidence.sampleRate != handoff.evidence.sampleRate)) {
            handoff.status = Mp4Mp3SampleTableHandoffStatus::Conflict;
            handoff.reason = Mp4Mp3SampleTableReason::IndependentExactAuthorityConflict;
            handoff.evidence = {};
            return handoff;
        }
        handoff.status = Mp4Mp3SampleTableHandoffStatus::Accepted;
        handoff.reason = Mp4Mp3SampleTableReason::ValidatedClassicSampleEditTables;
        return handoff;
    } catch (...) {
        handoff = {};
        handoff.reason = Mp4Mp3SampleTableReason::ProbeDocumentInvalid;
        return handoff;
    }
}

const char* mp4Mp3SampleTableStatusName(Mp4Mp3SampleTableStatus status) noexcept {
    switch (status) {
        case Mp4Mp3SampleTableStatus::Exact: return "exact";
        case Mp4Mp3SampleTableStatus::UnsupportedEarly: return "unsupported_early";
        case Mp4Mp3SampleTableStatus::UnsupportedLate: return "unsupported_late";
        case Mp4Mp3SampleTableStatus::Conflict: return "conflict";
        case Mp4Mp3SampleTableStatus::InvalidMedia: return "invalid_media";
        case Mp4Mp3SampleTableStatus::IoBudgetExceeded: return "io_budget_exceeded";
        case Mp4Mp3SampleTableStatus::IoError: return "io_error";
    }
    return "unsupported_early";
}

const char* mp4Mp3SampleTableReasonName(Mp4Mp3SampleTableReason reason) noexcept {
    switch (reason) {
        case Mp4Mp3SampleTableReason::NotEligible: return "not_eligible";
        case Mp4Mp3SampleTableReason::ValidatedClassicSampleEditTables: return "validated_classic_sample_edit_tables";
        case Mp4Mp3SampleTableReason::InputNotRegularFile: return "input_not_regular_file";
        case Mp4Mp3SampleTableReason::InputOpenFailed: return "input_open_failed";
        case Mp4Mp3SampleTableReason::InputReadFailed: return "input_read_failed";
        case Mp4Mp3SampleTableReason::InputSeekFailed: return "input_seek_failed";
        case Mp4Mp3SampleTableReason::InputChangedDuringProbe: return "input_changed_during_probe";
        case Mp4Mp3SampleTableReason::IoBudgetExceeded: return "io_budget_exceeded";
        case Mp4Mp3SampleTableReason::NotMovMp4: return "not_mov_mp4";
        case Mp4Mp3SampleTableReason::SelectedStreamAbsent: return "selected_stream_absent";
        case Mp4Mp3SampleTableReason::SelectedCodecNotMp3: return "selected_codec_not_mp3";
        case Mp4Mp3SampleTableReason::SelectedTrackIdInvalid: return "selected_track_id_invalid";
        case Mp4Mp3SampleTableReason::FragmentedMp4: return "fragmented_mp4";
        case Mp4Mp3SampleTableReason::MissingMoov: return "missing_moov";
        case Mp4Mp3SampleTableReason::DuplicateMoov: return "duplicate_moov";
        case Mp4Mp3SampleTableReason::MissingMdat: return "missing_mdat";
        case Mp4Mp3SampleTableReason::InvalidBox: return "invalid_box";
        case Mp4Mp3SampleTableReason::UnsupportedExtendedNestedBox: return "unsupported_extended_nested_box";
        case Mp4Mp3SampleTableReason::ParserResourceLimit: return "parser_resource_limit";
        case Mp4Mp3SampleTableReason::SelectedTrackAbsent: return "selected_track_absent";
        case Mp4Mp3SampleTableReason::DuplicateSelectedTrack: return "duplicate_selected_track";
        case Mp4Mp3SampleTableReason::SelectedTrackNotAudio: return "selected_track_not_audio";
        case Mp4Mp3SampleTableReason::InvalidMovieHeader: return "invalid_movie_header";
        case Mp4Mp3SampleTableReason::InvalidTrackHeader: return "invalid_track_header";
        case Mp4Mp3SampleTableReason::InvalidMediaHeader: return "invalid_media_header";
        case Mp4Mp3SampleTableReason::InvalidHandler: return "invalid_handler";
        case Mp4Mp3SampleTableReason::UnsupportedHeaderVersion: return "unsupported_header_version";
        case Mp4Mp3SampleTableReason::InvalidOrUnsupportedEditList: return "invalid_or_unsupported_edit_list";
        case Mp4Mp3SampleTableReason::EditDurationNotSampleExact: return "edit_duration_not_sample_exact";
        case Mp4Mp3SampleTableReason::EditTimelineConflict: return "edit_timeline_conflict";
        case Mp4Mp3SampleTableReason::UnsupportedSampleEntry: return "unsupported_sample_entry";
        case Mp4Mp3SampleTableReason::ProtectedSampleEntry: return "protected_sample_entry";
        case Mp4Mp3SampleTableReason::MultipleSampleDescriptions: return "multiple_sample_descriptions";
        case Mp4Mp3SampleTableReason::ExternalDataReference: return "external_data_reference";
        case Mp4Mp3SampleTableReason::InvalidDataReference: return "invalid_data_reference";
        case Mp4Mp3SampleTableReason::InvalidEsds: return "invalid_esds";
        case Mp4Mp3SampleTableReason::UnsupportedMp3ObjectType: return "unsupported_mp3_object_type";
        case Mp4Mp3SampleTableReason::UnsupportedSampleSizeTable: return "unsupported_sample_size_table";
        case Mp4Mp3SampleTableReason::UnsupportedChunkOffsetTable: return "unsupported_chunk_offset_table";
        case Mp4Mp3SampleTableReason::InvalidStts: return "invalid_stts";
        case Mp4Mp3SampleTableReason::InvalidStsz: return "invalid_stsz";
        case Mp4Mp3SampleTableReason::InvalidStsc: return "invalid_stsc";
        case Mp4Mp3SampleTableReason::InvalidChunkOffsets: return "invalid_chunk_offsets";
        case Mp4Mp3SampleTableReason::SampleCountConflict: return "sample_count_conflict";
        case Mp4Mp3SampleTableReason::DurationConflict: return "duration_conflict";
        case Mp4Mp3SampleTableReason::SampleDescriptionConflict: return "sample_description_conflict";
        case Mp4Mp3SampleTableReason::ChunkMappingConflict: return "chunk_mapping_conflict";
        case Mp4Mp3SampleTableReason::ChunkOutsideMdat: return "chunk_outside_mdat";
        case Mp4Mp3SampleTableReason::ChunkOverlap: return "chunk_overlap";
        case Mp4Mp3SampleTableReason::Mp3HeaderConflict: return "mp3_header_conflict";
        case Mp4Mp3SampleTableReason::UnsupportedMp3Profile: return "unsupported_mp3_profile";
        case Mp4Mp3SampleTableReason::VbrOrProfileChange: return "vbr_or_profile_change";
        case Mp4Mp3SampleTableReason::SampleSizeNotSingleCbrFrame: return "sample_size_not_single_cbr_frame";
        case Mp4Mp3SampleTableReason::TerminalDiscardConflict: return "terminal_discard_conflict";
        case Mp4Mp3SampleTableReason::IndependentExactAuthorityConflict: return "independent_exact_authority_conflict";
        case Mp4Mp3SampleTableReason::ProbeDocumentMissing: return "probe_document_missing";
        case Mp4Mp3SampleTableReason::ProbeDocumentInvalid: return "probe_document_invalid";
        case Mp4Mp3SampleTableReason::SourceIdentityMismatch: return "source_identity_mismatch";
    }
    return "not_eligible";
}

const char* mp4Mp3SampleTableHandoffStatusName(
    Mp4Mp3SampleTableHandoffStatus status) noexcept {
    switch (status) {
        case Mp4Mp3SampleTableHandoffStatus::Accepted: return "accepted";
        case Mp4Mp3SampleTableHandoffStatus::Unavailable: return "unavailable";
        case Mp4Mp3SampleTableHandoffStatus::Conflict: return "conflict";
    }
    return "unavailable";
}

}  // namespace AveMediaBridge::Probe
