#pragma once
#include <stdint.h>
#include <stddef.h>
#if defined(_WIN32)
#define AVEMEDIABRIDGE_CALL __stdcall
#else
#define AVEMEDIABRIDGE_CALL
#endif
#pragma pack(push, 8)
#define AVEMEDIABRIDGE_IMPORT_RESULT_CANCELED 4
#define AVEMEDIABRIDGE_WAVEFORM_CHUNK_FLAG_LONG_FORM_ENERGY 0x00000001u

typedef struct AveMediaBridgeImportProgress {
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
} AveMediaBridgeImportProgress;

typedef void (AVEMEDIABRIDGE_CALL *AveMediaBridgeProgressCallback)(
    const AveMediaBridgeImportProgress* progress,
    void* userData);

typedef int (AVEMEDIABRIDGE_CALL *AveMediaBridgeCancelCallback)(
    void* userData);

typedef struct AveMediaBridgeWaveformChunk {
    uint32_t structSize;
    uint64_t firstFrame;
    uint32_t framesPerBin;
    uint32_t binCount;
    uint32_t valuesPerBin;
    int sampleRate;
    int channels;
    const float* minMaxPairs;
    uint32_t flags;
    const double* sumSquaresPerBin;
    const double* sumAbsPerBin;
    const uint64_t* frameCountPerBin;
} AveMediaBridgeWaveformChunk;

typedef void (AVEMEDIABRIDGE_CALL *AveMediaBridgeWaveformChunkCallback)(
    const AveMediaBridgeWaveformChunk* chunk,
    void* userData);

typedef struct AveMediaBridgeImportOptions {
    uint32_t structSize;
    const wchar_t* inputPath;
    const wchar_t* sessionMediaDir;
    AveMediaBridgeProgressCallback onProgress;
    AveMediaBridgeCancelCallback shouldCancel;
    void* userData;
    AveMediaBridgeWaveformChunkCallback onWaveformChunk;
} AveMediaBridgeImportOptions;

#pragma pack(pop)
