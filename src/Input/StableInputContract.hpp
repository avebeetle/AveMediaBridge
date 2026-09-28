#pragma once

#include "AveMediaBridge/AveMediaBridgeInputApi.h"

namespace AveMediaBridge::Input {
AMBI_Status validateSource(const AMBI_SourceV1* source) noexcept;
AMBI_Status readSource(const AMBI_SourceV1& source, uint64_t offset,
    void* destination, uint32_t requestedBytes, uint32_t& bytesRead) noexcept;
}
