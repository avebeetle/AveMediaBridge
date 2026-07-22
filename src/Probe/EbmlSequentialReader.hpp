#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace AveMediaBridge::Probe::MatroskaAacDetail {

struct EbmlVint {
    bool valid = false;
    bool unknown = false;
    std::uint64_t value = 0;
    std::size_t length = 0;
};

struct EbmlElementHeader {
    bool valid = false;
    bool unknownSize = false;
    std::uint64_t id = 0;
    std::uint64_t size = 0;
    std::uint64_t offset = 0;
    std::uint64_t payloadOffset = 0;
    std::size_t headerBytes = 0;
};

class ForwardFileReader {
public:
    ForwardFileReader(
        const std::filesystem::path& path,
        std::size_t bufferBytes,
        std::uint64_t forceReadErrorAfterBytes =
            (std::numeric_limits<std::uint64_t>::max)());
    ~ForwardFileReader();

    ForwardFileReader(const ForwardFileReader&) = delete;
    ForwardFileReader& operator=(const ForwardFileReader&) = delete;

    bool valid() const noexcept;
    bool readExact(void* destination, std::size_t bytes);
    bool readByte(std::uint8_t& value);
    bool skip(std::uint64_t bytes);
    bool eof();
    bool fileUnchanged() const noexcept;
    void beginCrc32();
    bool endCrc32(std::uint32_t expected);

    std::uint64_t position() const noexcept;
    std::uint64_t fileSize() const noexcept;
    std::uint64_t bytesReturned() const noexcept;
    std::uint64_t readCalls() const noexcept;
    std::size_t bufferBytes() const noexcept;
    const std::string& error() const noexcept;

private:
    bool refill();

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::vector<std::uint8_t> buffer_;
    std::size_t cursor_ = 0;
    std::size_t available_ = 0;
    std::uint64_t logicalPosition_ = 0;
    std::uint64_t fileSize_ = 0;
    std::uint64_t bytesReturned_ = 0;
    std::uint64_t readCalls_ = 0;
    std::uint64_t forceReadErrorAfterBytes_ =
        (std::numeric_limits<std::uint64_t>::max)();
    bool physicalEof_ = false;
    BY_HANDLE_FILE_INFORMATION initialInfo_{};
    bool initialInfoValid_ = false;
    std::string error_;
    std::vector<std::uint32_t> crc32_;
};

EbmlVint readEbmlVint(ForwardFileReader& reader, bool preserveMarker);
bool readEbmlElementHeader(ForwardFileReader& reader, EbmlElementHeader& header);
EbmlVint readMemoryVint(
    const std::uint8_t* data,
    std::size_t size,
    std::size_t offset,
    bool preserveMarker) noexcept;

bool checkedAdd(std::uint64_t left, std::uint64_t right, std::uint64_t& result) noexcept;
std::uint64_t readUnsigned(const std::uint8_t* data, std::size_t size) noexcept;
std::int64_t readSigned(const std::uint8_t* data, std::size_t size) noexcept;
bool validateEbmlCrc32(
    const std::uint8_t* data,
    std::size_t size,
    std::uint32_t expected) noexcept;

}  // namespace AveMediaBridge::Probe::MatroskaAacDetail
