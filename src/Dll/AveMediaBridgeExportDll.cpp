#include "AveMediaBridge/AveMediaBridgeExportApi.h"
#include "Export/StreamingExportJob.hpp"

namespace {

AveMediaBridge::Export::ExportRegistry g_registry;

void setError(wchar_t* text, uint32_t capacity, const wchar_t* message) noexcept {
    if (capacity == 0 || !text) return;
    uint32_t i = 0;
    while (message[i] != L'\0' && i + 1 < capacity) {
        text[i] = message[i];
        ++i;
    }
    text[i] = L'\0';
}

const wchar_t* statusText(AMBE_Status status) noexcept {
    switch (status) {
    case AMBE_OK: return L"";
    case AMBE_INVALID_ARGUMENT: return L"invalid export argument";
    case AMBE_UNSUPPORTED: return L"streaming export writer unavailable";
    case AMBE_INVALID_STATE: return L"invalid export job state or handle";
    case AMBE_IO_ERROR: return L"export I/O error";
    case AMBE_FINALIZE_ERROR: return L"export finalization error";
    case AMBE_CANCELED: return L"export canceled";
    default: return L"internal export error";
    }
}

template <typename F>
AMBE_Status guarded(wchar_t* text, uint32_t capacity, F&& action) noexcept {
    if (capacity > 0 && !text) return AMBE_INVALID_ARGUMENT;
    setError(text, capacity, L"");
    try {
        const AMBE_Status status = action();
        setError(text, capacity, statusText(status));
        return status;
    } catch (...) {
        setError(text, capacity, statusText(AMBE_INTERNAL_ERROR));
        return AMBE_INTERNAL_ERROR;
    }
}

} // namespace

extern "C" {

AMBE_Status __cdecl AveMediaBridge_ExportQueryCapabilities(
    AMBE_CapabilitiesV1* capabilities, wchar_t* errorText, uint32_t errorCapacity) {
    return guarded(errorText, errorCapacity, [&]() -> AMBE_Status {
        if (AveMediaBridge::Export::validateCapabilities(capabilities) != AMBE_OK)
            return AMBE_INVALID_ARGUMENT;
        capabilities->profileBits = 0;
        capabilities->maxBlockBytes = AMBE_MAX_BLOCK_BYTES;
        return AMBE_OK;
    });
}

AMBE_Status __cdecl AveMediaBridge_ExportBegin(
    const wchar_t* scratchPath, const AMBE_InputV1* input, AMBE_Handle* outHandle,
    wchar_t* errorText, uint32_t errorCapacity) {
    if (outHandle) *outHandle = 0;
    return guarded(errorText, errorCapacity, [&]() -> AMBE_Status {
        if (!outHandle || !scratchPath || scratchPath[0] == L'\0' ||
            AveMediaBridge::Export::validateInput(input) != AMBE_OK)
            return AMBE_INVALID_ARGUMENT;
        // Task 2 installs a qualified writer. No scratch file is created here.
        return AMBE_UNSUPPORTED;
    });
}

AMBE_Status __cdecl AveMediaBridge_ExportWriteF32(
    AMBE_Handle handle, const float* samples, uint32_t frameCount,
    wchar_t* errorText, uint32_t errorCapacity) {
    return guarded(errorText, errorCapacity, [&]() -> AMBE_Status {
        const auto job = g_registry.find(handle);
        return job ? job->write(samples, frameCount) : AMBE_INVALID_STATE;
    });
}

AMBE_Status __cdecl AveMediaBridge_ExportFinish(
    AMBE_Handle handle, AMBE_ResultV1* result,
    wchar_t* errorText, uint32_t errorCapacity) {
    return guarded(errorText, errorCapacity, [&]() -> AMBE_Status {
        const auto job = g_registry.find(handle);
        return job ? job->finish(result) : AMBE_INVALID_STATE;
    });
}

AMBE_Status __cdecl AveMediaBridge_ExportAbort(
    AMBE_Handle handle, wchar_t* errorText, uint32_t errorCapacity) {
    return guarded(errorText, errorCapacity, [&]() -> AMBE_Status {
        const auto job = g_registry.find(handle);
        return job ? job->abort() : AMBE_INVALID_STATE;
    });
}

AMBE_Status __cdecl AveMediaBridge_ExportDestroy(
    AMBE_Handle handle, wchar_t* errorText, uint32_t errorCapacity) {
    return guarded(errorText, errorCapacity, [&]() -> AMBE_Status {
        return g_registry.destroy(handle);
    });
}

} // extern C
