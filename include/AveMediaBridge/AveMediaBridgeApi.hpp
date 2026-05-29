#pragma once

#if defined(_WIN32)
#if defined(AVEMEDIABRIDGE_BUILD_DLL)
#define AVEMEDIABRIDGE_C_API extern "C" __declspec(dllexport)
#else
#define AVEMEDIABRIDGE_C_API extern "C" __declspec(dllimport)
#endif
#else
#define AVEMEDIABRIDGE_C_API extern "C"
#endif

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

AVEMEDIABRIDGE_C_API const wchar_t* AveMediaBridge_GetVersion();
