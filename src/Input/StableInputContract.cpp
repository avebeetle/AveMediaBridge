#include "Input/StableInputContract.hpp"

#include <algorithm>
#include <iterator>
#include <limits>

namespace AveMediaBridge::Input {
AMBI_Status validateSource(const AMBI_SourceV1* source) noexcept {
    if (source == nullptr || source->structSize != sizeof(AMBI_SourceV1))
        return AMBI_INVALID_ARGUMENT;
    if (source->abiVersion != AMBI_ABI_VERSION || source->byteSize == 0 ||
        source->byteSize > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        source->readAt == nullptr || source->checkCancel == nullptr)
        return AMBI_INVALID_ARGUMENT;
    if (std::all_of(std::begin(source->sourceToken), std::end(source->sourceToken),
            [](uint8_t byte) { return byte == 0; }))
        return AMBI_INVALID_ARGUMENT;
    if (std::any_of(std::begin(source->reserved), std::end(source->reserved),
            [](uint32_t value) { return value != 0; }))
        return AMBI_INVALID_ARGUMENT;
    return AMBI_OK;
}

AMBI_Status readSource(const AMBI_SourceV1& source, uint64_t offset,
    void* destination, uint32_t requestedBytes,
    uint32_t& bytesRead) noexcept {
    bytesRead = 0;
    if (validateSource(&source) != AMBI_OK || requestedBytes > AMBI_MAX_READ_BYTES ||
        offset > source.byteSize || (requestedBytes != 0 && destination == nullptr))
        return AMBI_INVALID_ARGUMENT;
    if (requestedBytes == 0) return AMBI_OK;
    if (offset == source.byteSize) return AMBI_EOF;

    const auto remaining = source.byteSize - offset;
    const auto boundedBytes = static_cast<uint32_t>(
        std::min<uint64_t>(requestedBytes, remaining));
    try {
        const AMBI_Status cancel = source.checkCancel(source.user);
        if (cancel == AMBI_CANCELED || cancel == AMBI_IO_ERROR) return cancel;
        if (cancel != AMBI_OK) return AMBI_IO_ERROR;

        uint32_t callbackBytes = 0;
        const AMBI_Status status = source.readAt(source.user, offset,
            destination, boundedBytes, &callbackBytes);
        if (callbackBytes > boundedBytes) return AMBI_IO_ERROR;
        if (status == AMBI_CANCELED) return AMBI_CANCELED;
        if (status == AMBI_IO_ERROR) return AMBI_IO_ERROR;
        if (status != AMBI_OK && status != AMBI_EOF) return AMBI_IO_ERROR;
        if (callbackBytes != boundedBytes) return AMBI_IO_ERROR;
        if (status == AMBI_EOF && remaining != boundedBytes) return AMBI_IO_ERROR;
        bytesRead = callbackBytes;
        return boundedBytes < requestedBytes ? AMBI_EOF : AMBI_OK;
    } catch (...) {
        return AMBI_IO_ERROR;
    }
}
}
