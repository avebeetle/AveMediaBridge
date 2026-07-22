#include "EbmlSequentialReader.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace AveMediaBridge::Probe::MatroskaAacDetail {
namespace {

using CrcTables = std::array<std::array<std::uint32_t, 256>, 8>;

const CrcTables& crcTables() noexcept {
    static const CrcTables tables = [] {
        CrcTables values{};
        for (std::uint32_t index = 0; index < values[0].size(); ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value >> 1) ^ ((value & 1U) ? 0xEDB88320U : 0U);
            }
            values[0][index] = value;
        }
        for (std::size_t slice = 1; slice < values.size(); ++slice) {
            for (std::size_t index = 0; index < values[slice].size(); ++index) {
                const std::uint32_t previous = values[slice - 1][index];
                values[slice][index] =
                    values[0][previous & 0xFFU] ^ (previous >> 8);
            }
        }
        return values;
    }();
    return tables;
}

std::uint32_t updateCrc32(
    std::uint32_t crc,
    const std::uint8_t* data,
    std::size_t size) noexcept {
    const auto& tables = crcTables();

    while (size >= 8) {
        std::uint32_t first = 0;
        std::uint32_t second = 0;
        std::memcpy(&first, data, sizeof(first));
        std::memcpy(&second, data + sizeof(first), sizeof(second));
        first ^= crc;
        crc = tables[7][first & 0xFFU] ^
            tables[6][(first >> 8) & 0xFFU] ^
            tables[5][(first >> 16) & 0xFFU] ^
            tables[4][first >> 24] ^
            tables[3][second & 0xFFU] ^
            tables[2][(second >> 8) & 0xFFU] ^
            tables[1][(second >> 16) & 0xFFU] ^
            tables[0][second >> 24];
        data += 8;
        size -= 8;
    }
    while (size-- > 0) {
        crc = tables[0][(crc ^ *data++) & 0xFFU] ^ (crc >> 8);
    }
    return crc;
}

}  // namespace

ForwardFileReader::ForwardFileReader(
    const std::filesystem::path& path,
    std::size_t bufferBytes,
    std::uint64_t forceReadErrorAfterBytes)
    : buffer_((std::max)(bufferBytes, std::size_t{1})),
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
        error_ = "open_failed";
        return;
    }
    LARGE_INTEGER size{};
    if (GetFileType(handle_) != FILE_TYPE_DISK ||
        !GetFileSizeEx(handle_, &size) || size.QuadPart < 0) {
        error_ = "file_info_failed";
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        return;
    }
    fileSize_ = static_cast<std::uint64_t>(size.QuadPart);
    initialInfoValid_ =
        GetFileInformationByHandle(handle_, &initialInfo_) != FALSE;
    if (!initialInfoValid_) {
        error_ = "file_info_failed";
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
}

ForwardFileReader::~ForwardFileReader() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
    }
}

bool ForwardFileReader::valid() const noexcept {
    return handle_ != INVALID_HANDLE_VALUE && error_.empty();
}

bool ForwardFileReader::refill() {
    if (!valid() || physicalEof_) {
        return false;
    }
    if (bytesReturned_ >= forceReadErrorAfterBytes_) {
        error_ = "forced_read_error";
        return false;
    }
    const std::uint64_t remainingBeforeForcedError =
        forceReadErrorAfterBytes_ - bytesReturned_;
    const std::size_t requested = static_cast<std::size_t>(
        (std::min<std::uint64_t>)(
            buffer_.size(),
            (std::min<std::uint64_t>)(
                remainingBeforeForcedError,
                (std::numeric_limits<DWORD>::max)())));
    if (requested == 0) {
        error_ = "forced_read_error";
        return false;
    }
    DWORD returned = 0;
    ++readCalls_;
    if (!ReadFile(
            handle_,
            buffer_.data(),
            static_cast<DWORD>(requested),
            &returned,
            nullptr)) {
        error_ = "read_failed";
        return false;
    }
    cursor_ = 0;
    available_ = returned;
    bytesReturned_ += returned;
    if (returned == 0) {
        physicalEof_ = true;
        return false;
    }
    return true;
}

bool ForwardFileReader::readExact(void* destination, std::size_t bytes) {
    auto* output = static_cast<std::uint8_t*>(destination);
    while (bytes > 0) {
        if (cursor_ == available_ && !refill()) {
            if (error_.empty()) {
                error_ = "unexpected_eof";
            }
            return false;
        }
        const std::size_t take = (std::min)(bytes, available_ - cursor_);
        for (std::uint32_t& crc : crc32_) {
            crc = updateCrc32(crc, buffer_.data() + cursor_, take);
        }
        if (output) {
            std::memcpy(output, buffer_.data() + cursor_, take);
            output += take;
        }
        cursor_ += take;
        logicalPosition_ += take;
        bytes -= take;
    }
    return true;
}

bool ForwardFileReader::readByte(std::uint8_t& value) {
    if (cursor_ == available_ && !refill()) {
        if (error_.empty()) {
            error_ = "unexpected_eof";
        }
        return false;
    }
    value = buffer_[cursor_++];
    const auto& table = crcTables()[0];
    for (std::uint32_t& crc : crc32_) {
        crc = table[(crc ^ value) & 0xFFU] ^ (crc >> 8);
    }
    ++logicalPosition_;
    return true;
}

bool ForwardFileReader::skip(std::uint64_t bytes) {
    while (bytes > 0) {
        const std::size_t take = static_cast<std::size_t>((std::min<std::uint64_t>)(
            bytes, (std::numeric_limits<std::size_t>::max)()));
        if (!readExact(nullptr, take)) {
            return false;
        }
        bytes -= take;
    }
    return true;
}

bool ForwardFileReader::eof() {
    if (!valid()) {
        return false;
    }
    if (logicalPosition_ != fileSize_) {
        return false;
    }
    if (cursor_ != available_) {
        return false;
    }
    if (!physicalEof_) {
        std::uint8_t ignored = 0;
        DWORD returned = 0;
        ++readCalls_;
        if (!ReadFile(handle_, &ignored, 1, &returned, nullptr)) {
            error_ = "read_failed";
            return false;
        }
        bytesReturned_ += returned;
        physicalEof_ = returned == 0;
    }
    return physicalEof_ && error_.empty();
}

bool ForwardFileReader::fileUnchanged() const noexcept {
    if (!valid() || !initialInfoValid_) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION current{};
    if (!GetFileInformationByHandle(handle_, &current)) {
        return false;
    }
    return current.dwVolumeSerialNumber == initialInfo_.dwVolumeSerialNumber &&
        current.nFileIndexHigh == initialInfo_.nFileIndexHigh &&
        current.nFileIndexLow == initialInfo_.nFileIndexLow &&
        current.nFileSizeHigh == initialInfo_.nFileSizeHigh &&
        current.nFileSizeLow == initialInfo_.nFileSizeLow &&
        current.ftLastWriteTime.dwHighDateTime ==
            initialInfo_.ftLastWriteTime.dwHighDateTime &&
        current.ftLastWriteTime.dwLowDateTime ==
            initialInfo_.ftLastWriteTime.dwLowDateTime;
}

void ForwardFileReader::beginCrc32() {
    crc32_.push_back(0xFFFFFFFFU);
}

bool ForwardFileReader::endCrc32(std::uint32_t expected) {
    if (crc32_.empty()) {
        return false;
    }
    const std::uint32_t actual = crc32_.back() ^ 0xFFFFFFFFU;
    crc32_.pop_back();
    return actual == expected;
}

std::uint64_t ForwardFileReader::position() const noexcept { return logicalPosition_; }
std::uint64_t ForwardFileReader::fileSize() const noexcept { return fileSize_; }
std::uint64_t ForwardFileReader::bytesReturned() const noexcept { return bytesReturned_; }
std::uint64_t ForwardFileReader::readCalls() const noexcept { return readCalls_; }
std::size_t ForwardFileReader::bufferBytes() const noexcept { return buffer_.size(); }
const std::string& ForwardFileReader::error() const noexcept { return error_; }

EbmlVint readEbmlVint(ForwardFileReader& reader, bool preserveMarker) {
    EbmlVint result;
    std::uint8_t first = 0;
    if (!reader.readByte(first) || first == 0) {
        return result;
    }
    std::uint8_t marker = 0x80;
    std::size_t length = 1;
    while ((first & marker) == 0 && length <= 8) {
        marker >>= 1;
        ++length;
    }
    if (length > 8) {
        return result;
    }
    std::uint64_t value = preserveMarker ? first : first & (marker - 1U);
    for (std::size_t index = 1; index < length; ++index) {
        std::uint8_t byte = 0;
        if (!reader.readByte(byte)) {
            return result;
        }
        value = (value << 8) | byte;
    }
    result.valid = true;
    result.length = length;
    result.value = value;
    result.unknown = !preserveMarker &&
        value == ((std::uint64_t{1} << (7U * length)) - 1U);
    return result;
}

bool readEbmlElementHeader(ForwardFileReader& reader, EbmlElementHeader& header) {
    header = {};
    header.offset = reader.position();
    const EbmlVint id = readEbmlVint(reader, true);
    const EbmlVint size = id.valid ? readEbmlVint(reader, false) : EbmlVint{};
    if (!id.valid || !size.valid) {
        return false;
    }
    header.valid = true;
    header.id = id.value;
    header.size = size.value;
    header.unknownSize = size.unknown;
    header.headerBytes = id.length + size.length;
    header.payloadOffset = reader.position();
    return true;
}

EbmlVint readMemoryVint(
    const std::uint8_t* data,
    std::size_t size,
    std::size_t offset,
    bool preserveMarker) noexcept {
    EbmlVint result;
    if (!data || offset >= size || data[offset] == 0) {
        return result;
    }
    const std::uint8_t first = data[offset];
    std::uint8_t marker = 0x80;
    std::size_t length = 1;
    while ((first & marker) == 0 && length <= 8) {
        marker >>= 1;
        ++length;
    }
    if (length > 8 || length > size - offset) {
        return result;
    }
    std::uint64_t value = preserveMarker ? first : first & (marker - 1U);
    for (std::size_t index = 1; index < length; ++index) {
        value = (value << 8) | data[offset + index];
    }
    result.valid = true;
    result.value = value;
    result.length = length;
    result.unknown = !preserveMarker &&
        value == ((std::uint64_t{1} << (7U * length)) - 1U);
    return result;
}

bool checkedAdd(std::uint64_t left, std::uint64_t right, std::uint64_t& result) noexcept {
    if (left > (std::numeric_limits<std::uint64_t>::max)() - right) {
        return false;
    }
    result = left + right;
    return true;
}

std::uint64_t readUnsigned(const std::uint8_t* data, std::size_t size) noexcept {
    if (!data || size == 0 || size > 8) {
        return 0;
    }
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < size; ++index) {
        value = (value << 8) | data[index];
    }
    return value;
}

std::int64_t readSigned(const std::uint8_t* data, std::size_t size) noexcept {
    if (!data || size == 0 || size > 8) {
        return 0;
    }
    std::uint64_t value = readUnsigned(data, size);
    if (size < 8 && (data[0] & 0x80U) != 0) {
        value |= ~((std::uint64_t{1} << (size * 8U)) - 1U);
    }
    return static_cast<std::int64_t>(value);
}

bool validateEbmlCrc32(
    const std::uint8_t* data,
    std::size_t size,
    std::uint32_t expected) noexcept {
    return (updateCrc32(0xFFFFFFFFU, data, size) ^ 0xFFFFFFFFU) == expected;
}

}  // namespace AveMediaBridge::Probe::MatroskaAacDetail
