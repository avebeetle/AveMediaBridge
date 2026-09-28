#include "AveMediaBridge/AveMediaBridgeExportApi.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <cstdlib>

struct Check {
    bool ok = true;
    void expect(bool condition, const char* message) {
        if (!condition) { std::cerr << "FAIL: " << message << '\n'; ok = false; }
    }
};

int main() {
    Check check;
    check.expect(sizeof(AMBE_InputV1) == 48, "v1 input layout");
    check.expect(offsetof(AMBE_InputV1, expectedFrames) == 24, "frame offset");
    check.expect(sizeof(AMBE_CapabilitiesV1) == 32 && sizeof(AMBE_ResultV1) == 32, "output layout");
    check.expect(offsetof(AMBE_CapabilitiesV1, profileBits) == 8, "capability bits offset");
    check.expect(offsetof(AMBE_ResultV1, acceptedFrames) == 8, "result frames offset");

    AMBE_CapabilitiesV1 caps{};
    caps.structSize = sizeof(caps); caps.abiVersion = AMBE_ABI_VERSION;
    wchar_t tiny[1] = {L'X'};
    check.expect(AveMediaBridge_ExportQueryCapabilities(&caps, tiny, 1) == AMBE_OK,
        "valid capability query succeeds");
    check.expect(tiny[0] == L'\0', "one-character diagnostic buffer terminates");
    const bool expectedWriter = std::getenv("AMBE_EXPECT_FLOAT_WAV") != nullptr;
    check.expect(caps.profileBits == (expectedWriter ? AMBE_PROFILE_BIT_WAV_F32_NATIVE_V1 : 0),
        "capability matches qualified runtime");
    check.expect(caps.maxBlockBytes == AMBE_MAX_BLOCK_BYTES, "block limit is reported");
    check.expect(AveMediaBridge_ExportQueryCapabilities(&caps, nullptr, 1) == AMBE_INVALID_ARGUMENT,
        "positive diagnostic capacity needs a buffer");
    caps.reserved[0] = 1;
    check.expect(AveMediaBridge_ExportQueryCapabilities(&caps, nullptr, 0) == AMBE_INVALID_ARGUMENT,
        "reserved capability fields are checked");

    AMBE_InputV1 input{};
    input.structSize = sizeof(input); input.abiVersion = AMBE_ABI_VERSION;
    input.sampleRate = 48000; input.channels = 2; input.layout = AMBE_LAYOUT_STEREO_LR;
    input.profile = AMBE_PROFILE_WAV_F32_NATIVE_V1; input.expectedFrames = 3;
    AMBE_Handle handle = 99;
    check.expect(AveMediaBridge_ExportBegin(L"scratch.wav", &input, &handle, nullptr, 0) ==
        (expectedWriter ? AMBE_IO_ERROR : AMBE_UNSUPPORTED) && handle == 0,
        "valid Begin reports runtime availability or missing scratch");
    input.structSize = 0; handle = 99;
    check.expect(AveMediaBridge_ExportBegin(L"scratch.wav", &input, &handle, nullptr, 0) == AMBE_INVALID_ARGUMENT && handle == 0,
        "malformed size clears handle");
    input.structSize = sizeof(input); input.abiVersion = 2; handle = 99;
    check.expect(AveMediaBridge_ExportBegin(L"scratch.wav", &input, &handle, nullptr, 0) == AMBE_INVALID_ARGUMENT && handle == 0,
        "malformed version clears handle");
    input.abiVersion = AMBE_ABI_VERSION; input.reserved[0] = 1; handle = 99;
    check.expect(AveMediaBridge_ExportBegin(L"scratch.wav", &input, &handle, nullptr, 0) == AMBE_INVALID_ARGUMENT && handle == 0,
        "malformed reserved field clears handle");
    input.reserved[0] = 0; input.layout = 0; handle = 99;
    check.expect(AveMediaBridge_ExportBegin(L"scratch.wav", &input, &handle, nullptr, 0) == AMBE_INVALID_ARGUMENT && handle == 0,
        "unknown layout clears handle");
    input.layout = AMBE_LAYOUT_STEREO_LR; input.expectedFrames = 0; handle = 99;
    check.expect(AveMediaBridge_ExportBegin(L"scratch.wav", &input, &handle, nullptr, 0) == AMBE_INVALID_ARGUMENT && handle == 0,
        "empty export is invalid");
    input.expectedFrames = 3; input.sampleRate = 1073741824u; handle = 99;
    check.expect(AveMediaBridge_ExportBegin(L"scratch.wav", &input, &handle, nullptr, 0) == AMBE_INVALID_ARGUMENT && handle == 0,
        "WAV byte-rate overflow is invalid");
    input.sampleRate = 48000; input.expectedFrames = static_cast<uint64_t>(INT64_MAX / 8) + 1; handle = 99;
    check.expect(AveMediaBridge_ExportBegin(L"scratch.wav", &input, &handle, nullptr, 0) == AMBE_INVALID_ARGUMENT && handle == 0,
        "container offset overflow is invalid");
    check.expect(AveMediaBridge_ExportDestroy(0, nullptr, 0) == AMBE_INVALID_STATE,
        "zero token is invalid");
    check.expect(AveMediaBridge_ExportWriteF32(0, nullptr, 1, nullptr, 0) == AMBE_INVALID_STATE,
        "zero token write is invalid");
    return check.ok ? 0 : 1;
}
