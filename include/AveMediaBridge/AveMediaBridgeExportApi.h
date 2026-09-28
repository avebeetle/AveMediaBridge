#ifndef AVEMEDIABRIDGE_EXPORT_API_H
#define AVEMEDIABRIDGE_EXPORT_API_H

#include <stdint.h>
#include <wchar.h>

#if !defined(_WIN64)
#error AveMediaBridge export ABI v1 supports Windows x64 only
#endif

#if defined(AVEMEDIABRIDGE_BUILD_DLL)
#define AMBE_API __declspec(dllexport)
#else
#define AMBE_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t AMBE_Handle;
typedef uint32_t AMBE_Status;

#define AMBE_ABI_VERSION 1u
#define AMBE_PROFILE_WAV_F32_NATIVE_V1 1u
#define AMBE_PROFILE_BIT_WAV_F32_NATIVE_V1 (UINT64_C(1) << 0)
#define AMBE_LAYOUT_MONO 1u
#define AMBE_LAYOUT_STEREO_LR 2u
#define AMBE_MAX_BLOCK_BYTES 1048576u

#define AMBE_OK 0u
#define AMBE_INVALID_ARGUMENT 1u
#define AMBE_UNSUPPORTED 2u
#define AMBE_INVALID_STATE 3u
#define AMBE_IO_ERROR 4u
#define AMBE_FINALIZE_ERROR 5u
#define AMBE_CANCELED 6u
#define AMBE_INTERNAL_ERROR 7u

#pragma pack(push, 8)
typedef struct AMBE_InputV1 {
    uint32_t structSize, abiVersion, sampleRate, channels, layout, profile;
    uint64_t expectedFrames;
    uint32_t reserved[4];
} AMBE_InputV1;

typedef struct AMBE_CapabilitiesV1 {
    uint32_t structSize, abiVersion;
    uint64_t profileBits;
    uint32_t maxBlockBytes, reserved[3];
} AMBE_CapabilitiesV1;

typedef struct AMBE_ResultV1 {
    uint32_t structSize, abiVersion;
    uint64_t acceptedFrames, encodedFrames;
    uint32_t reserved[2];
} AMBE_ResultV1;
#pragma pack(pop)

AMBE_API AMBE_Status __cdecl AveMediaBridge_ExportQueryCapabilities(
    AMBE_CapabilitiesV1* capabilities, wchar_t* errorText, uint32_t errorCapacity);
AMBE_API AMBE_Status __cdecl AveMediaBridge_ExportBegin(
    const wchar_t* scratchPath, const AMBE_InputV1* input, AMBE_Handle* outHandle,
    wchar_t* errorText, uint32_t errorCapacity);
AMBE_API AMBE_Status __cdecl AveMediaBridge_ExportWriteF32(
    AMBE_Handle handle, const float* samples, uint32_t frameCount,
    wchar_t* errorText, uint32_t errorCapacity);
AMBE_API AMBE_Status __cdecl AveMediaBridge_ExportFinish(
    AMBE_Handle handle, AMBE_ResultV1* result,
    wchar_t* errorText, uint32_t errorCapacity);
AMBE_API AMBE_Status __cdecl AveMediaBridge_ExportAbort(
    AMBE_Handle handle, wchar_t* errorText, uint32_t errorCapacity);
AMBE_API AMBE_Status __cdecl AveMediaBridge_ExportDestroy(
    AMBE_Handle handle, wchar_t* errorText, uint32_t errorCapacity);

#ifdef __cplusplus
}
#endif
#endif
