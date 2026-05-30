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

#define AVEMEDIABRIDGE_IMPORT_RESULT_CANCELED 4

struct AveMediaBridgeImportProgress {
    uint32_t structSize;
    uint64_t framesWritten;
    uint64_t bytesWritten;
    uint64_t estimatedTotalFrames;
    uint64_t estimatedTotalBytes;
    double availableEndSec;
    double progress01;
    int sampleRate;
    int channels;
    uint32_t flags;
};

typedef void (AVEMEDIABRIDGE_CALL *AveMediaBridgeProgressCallback)(
    const AveMediaBridgeImportProgress* progress,
    void* userData);

typedef int (AVEMEDIABRIDGE_CALL *AveMediaBridgeCancelCallback)(
    void* userData);

struct AveMediaBridgeImportOptions {
    uint32_t structSize;
    const wchar_t* inputPath;
    const wchar_t* sessionMediaDir;
    AveMediaBridgeProgressCallback onProgress;
    AveMediaBridgeCancelCallback shouldCancel;
    void* userData;
};

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

AVEMEDIABRIDGE_C_API int AveMediaBridge_ImportAudioToSession(
    const wchar_t* inputPath,
    const wchar_t* sessionMediaDir);

AVEMEDIABRIDGE_C_API int AveMediaBridge_ImportAudioToSessionEx(
    const AveMediaBridgeImportOptions* options);

AVEMEDIABRIDGE_C_API const wchar_t* AveMediaBridge_GetVersion();
