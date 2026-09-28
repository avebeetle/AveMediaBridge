#include "AveMediaBridge/AveMediaBridgeReaderApi.h"
#include <stddef.h>
_Static_assert(sizeof(AMBR_PrepareOptionsV1) == 40, "prepare size");
_Static_assert(sizeof(AMBR_ImportOptionsV1) == 64, "import size");
#define OFFSET(T, F, N) _Static_assert(offsetof(T, F) == N, #T "." #F)
OFFSET(AMBR_PrepareOptionsV1, structSize, 0);
OFFSET(AMBR_PrepareOptionsV1, abiVersion, 4);
OFFSET(AMBR_PrepareOptionsV1, source, 8);
OFFSET(AMBR_PrepareOptionsV1, displayLabel, 16);
OFFSET(AMBR_PrepareOptionsV1, reserved, 24);
OFFSET(AMBR_ImportOptionsV1, structSize, 0);
OFFSET(AMBR_ImportOptionsV1, abiVersion, 4);
OFFSET(AMBR_ImportOptionsV1, sessionMediaDir, 8);
OFFSET(AMBR_ImportOptionsV1, onProgress, 16);
OFFSET(AMBR_ImportOptionsV1, shouldCancel, 24);
OFFSET(AMBR_ImportOptionsV1, userData, 32);
OFFSET(AMBR_ImportOptionsV1, onWaveformChunk, 40);
OFFSET(AMBR_ImportOptionsV1, reserved, 48);
_Static_assert(sizeof(AveMediaBridgeImportOptions) == 56, "legacy options");
_Static_assert(sizeof(AveMediaBridgeImportProgress) == 72, "legacy progress");
_Static_assert(sizeof(AveMediaBridgeWaveformChunk) == 80, "legacy waveform");
OFFSET(AveMediaBridgeImportOptions, structSize, 0);
OFFSET(AveMediaBridgeImportOptions, inputPath, 8);
OFFSET(AveMediaBridgeImportOptions, sessionMediaDir, 16);
OFFSET(AveMediaBridgeImportOptions, onProgress, 24);
OFFSET(AveMediaBridgeImportOptions, shouldCancel, 32);
OFFSET(AveMediaBridgeImportOptions, userData, 40);
OFFSET(AveMediaBridgeImportOptions, onWaveformChunk, 48);
OFFSET(AveMediaBridgeImportProgress, structSize, 0);
OFFSET(AveMediaBridgeImportProgress, framesWritten, 8);
OFFSET(AveMediaBridgeImportProgress, bytesWritten, 16);
OFFSET(AveMediaBridgeImportProgress, estimatedTotalFrames, 24);
OFFSET(AveMediaBridgeImportProgress, estimatedTotalBytes, 32);
OFFSET(AveMediaBridgeImportProgress, availableEndSec, 40);
OFFSET(AveMediaBridgeImportProgress, progress01, 48);
OFFSET(AveMediaBridgeImportProgress, sampleRate, 56);
OFFSET(AveMediaBridgeImportProgress, channels, 60);
OFFSET(AveMediaBridgeImportProgress, flags, 64);
OFFSET(AveMediaBridgeWaveformChunk, structSize, 0);
OFFSET(AveMediaBridgeWaveformChunk, firstFrame, 8);
OFFSET(AveMediaBridgeWaveformChunk, framesPerBin, 16);
OFFSET(AveMediaBridgeWaveformChunk, binCount, 20);
OFFSET(AveMediaBridgeWaveformChunk, valuesPerBin, 24);
OFFSET(AveMediaBridgeWaveformChunk, sampleRate, 28);
OFFSET(AveMediaBridgeWaveformChunk, channels, 32);
OFFSET(AveMediaBridgeWaveformChunk, minMaxPairs, 40);
OFFSET(AveMediaBridgeWaveformChunk, flags, 48);
OFFSET(AveMediaBridgeWaveformChunk, sumSquaresPerBin, 56);
OFFSET(AveMediaBridgeWaveformChunk, sumAbsPerBin, 64);
OFFSET(AveMediaBridgeWaveformChunk, frameCountPerBin, 72);
#ifndef __cplusplus
_Static_assert(_Generic((AveMediaBridgeProgressCallback)0,
                   void(__stdcall *)(const AveMediaBridgeImportProgress *, void *): 1,
                   default: 0),
               "progress callback");
_Static_assert(_Generic((AveMediaBridgeCancelCallback)0, int(__stdcall *)(void *): 1, default: 0),
               "cancel callback");
_Static_assert(_Generic((AveMediaBridgeWaveformChunkCallback)0,
                   void(__stdcall *)(const AveMediaBridgeWaveformChunk *, void *): 1,
                   default: 0),
               "waveform callback");
_Static_assert(_Generic(&AveMediaBridge_ReaderPrepareV1,
                   int(__cdecl *)(const AMBR_PrepareOptionsV1 *, AMBR_PreparedInput **): 1,
                   default: 0),
               "prepare export");
#endif
int readerAbiC(void) { return 1; }
