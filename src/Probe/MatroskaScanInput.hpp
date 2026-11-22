#pragma once

#include <cstddef>
#include <cstdint>

namespace AveMediaBridge::Probe {

// Optional observation of the bytes returned by the sequential reader.
// Neither the Win32 handle nor the callback context transfers ownership.
struct MatroskaScanInput {
    void* pinnedFile;
    void* context;
    bool (*onRead)(void*, const std::uint8_t*, std::size_t);
};

}  // namespace AveMediaBridge::Probe
