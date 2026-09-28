#pragma once
#include "AveMediaBridgeInputApi.h"
#include "AveMediaBridgeImportTypes.h"

#define AMBR_ABI_VERSION 1u
#define AMBR_OK 0
#define AMBR_INVALID_ARGUMENT 1
#define AMBR_INPUT_FAILED 2
#define AMBR_OUTPUT_FAILED 3
#define AMBR_CANCELED 4
#define AMBR_UNSUPPORTED 5
#define AMBR_BINDING_MISMATCH 6
#define AMBR_WRONG_STATE 7
#define AMBR_INTERNAL_ERROR 99

#ifdef __cplusplus
extern "C" {
#endif
#if defined(AVEMEDIABRIDGE_BUILD_DLL)
#define AMBR_API __declspec(dllexport)
#else
#define AMBR_API __declspec(dllimport)
#endif
typedef struct AMBR_PreparedInput AMBR_PreparedInput;
#pragma pack(push, 8)
typedef struct AMBR_PrepareOptionsV1 {
    uint32_t structSize, abiVersion;
    const AMBI_SourceV1* source;
    const wchar_t* displayLabel;
    uint32_t reserved[4];
} AMBR_PrepareOptionsV1;
typedef struct AMBR_ImportOptionsV1 {
    uint32_t structSize, abiVersion;
    const wchar_t* sessionMediaDir;
    AveMediaBridgeProgressCallback onProgress;
    AveMediaBridgeCancelCallback shouldCancel;
    void* userData;
    AveMediaBridgeWaveformChunkCallback onWaveformChunk;
    uint32_t reserved[4];
} AMBR_ImportOptionsV1;
#pragma pack(pop)
/* Descriptor/label are copied; callback owner and module are borrowed until
   destroy. Operations on a handle are synchronous, serialized, non-reentrant.
   One valid import attempt consumes the handle. Probe JSON is diagnostic only. */
AMBR_API int __cdecl AveMediaBridge_ReaderPrepareV1(const AMBR_PrepareOptionsV1*, AMBR_PreparedInput**);
AMBR_API int __cdecl AveMediaBridge_ReaderWriteProbeJsonV1(AMBR_PreparedInput*, const wchar_t*);
AMBR_API int __cdecl AveMediaBridge_ReaderImportV1(AMBR_PreparedInput*, const AMBR_ImportOptionsV1*);
AMBR_API void __cdecl AveMediaBridge_ReaderDestroyV1(AMBR_PreparedInput*);
#ifdef __cplusplus
}
#endif
