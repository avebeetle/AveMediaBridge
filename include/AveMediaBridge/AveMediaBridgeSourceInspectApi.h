#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(AVEMEDIABRIDGE_BUILD_DLL)
#define AMBM_API __declspec(dllexport)
#else
#define AMBM_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef int (__cdecl *AMBM_Cancel)(void*);

/* Optional combined Matroska source inspection. pinned_file is a borrowed Win32 disk-file handle
   that denies write/delete sharing. The caller retains it through import.
   Callback runs on the calling thread. Return 1 publishes all outputs, 0
   requests ordinary fallback, -1 is cancellation, -2 is invalid arguments,
   source I/O or an internal failure. Non-success leaves all outputs untouched. */
AMBM_API int __cdecl AveMediaBridge_InspectMatroskaAacSourceV1(
    const wchar_t* path, void* pinned_file, AMBM_Cancel cancel, void* context,
    uint8_t* sha256_32, uint64_t* bytes, uint64_t* frames,
    int32_t* stream_index, int32_t* sample_rate, int32_t* channels);

/* Optional combined CAF/ALAC inspection of the same kind of pinned handle.
   Counts decoded samples per channel for qualified mono and stereo streams.
   Return 1 publishes all fields. Return 2 publishes only sha256_32 and bytes
   after completing the source hash with inconclusive packet evidence. Return
   0 requests ordinary fallback before hashing. -1 indicates cancellation,
   -2 an argument, I/O or internal error. For 0/-1/-2 no outputs are changed.
   path labels the import. All metadata and payload reads use pinned_file. */
AMBM_API int __cdecl AveMediaBridge_InspectCafAlacSourceV1(
    const wchar_t* path, void* pinned_file, AMBM_Cancel cancel, void* context,
    uint8_t* sha256_32, uint64_t* bytes, uint64_t* frames,
    int32_t* stream_index, int32_t* sample_rate, int32_t* channels);

#ifdef __cplusplus
}
#endif
