#pragma once

#include <stdint.h>

#if defined(_WIN32)
#if defined(AVEMEDIABRIDGE_BUILD_DLL)
#define AVEMEDIABRIDGE_C_API extern "C" __declspec(dllexport)
#else
#define AVEMEDIABRIDGE_C_API extern "C" __declspec(dllimport)
#endif
#define AVEMEDIABRIDGE_CALL __stdcall
#else
#define AVEMEDIABRIDGE_C_API extern "C"
#define AVEMEDIABRIDGE_CALL
#endif

#include "AveMediaBridgeImportTypes.h"

AVEMEDIABRIDGE_C_API int AveMediaBridge_TransformToWav(
    const wchar_t* inputPath,
    const wchar_t* outputWavPath,
    float gain);

AVEMEDIABRIDGE_C_API int AveMediaBridge_GetVersionString(
    wchar_t* outBuffer,
    int outBufferChars);

AVEMEDIABRIDGE_C_API int AveMediaBridge_GetLastErrorText(
    wchar_t* outBuffer,
    int outBufferChars);

AVEMEDIABRIDGE_C_API int AveMediaBridge_ProbeToJson(
    const wchar_t* inputPath,
    const wchar_t* outputJsonPath);

AVEMEDIABRIDGE_C_API int AveMediaBridge_ProbeFrameCountCandidatesToJson(
    const wchar_t* inputPath,
    const wchar_t* outputJsonPath);

AVEMEDIABRIDGE_C_API int AveMediaBridge_ImportAudioToSession(
    const wchar_t* inputPath,
    const wchar_t* sessionMediaDir);

AVEMEDIABRIDGE_C_API int AveMediaBridge_ImportAudioToSessionEx(
    const AveMediaBridgeImportOptions* options);

// Request a fresh, append-only PCM output on local NTFS. Other supported
// volumes still decode, with no file-ID proof. Existing output is refused.
AVEMEDIABRIDGE_C_API int __cdecl AveMediaBridge_ImportAudioToSessionGuardedV1(
    const AveMediaBridgeImportOptions* options);
// One on the active guarded progress callback thread, otherwise zero. Outputs
// are borrowed by the call; only written on success. The caller must compare
// all three fields to its pinned read handle and to the final file identity.
AVEMEDIABRIDGE_C_API int __cdecl AveMediaBridge_GuardedPcmFileIdV1(
    unsigned long* volume, unsigned long* indexHigh, unsigned long* indexLow);

AVEMEDIABRIDGE_C_API const wchar_t* AveMediaBridge_GetVersion();
