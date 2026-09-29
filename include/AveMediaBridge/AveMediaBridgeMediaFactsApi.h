#pragma once
#include "AveMediaBridgeReaderApi.h"

#define AMBM_BUFFER_TOO_SMALL 8
#define AMBM_MAX_JSON_BYTES 65536u

#ifdef __cplusplus
extern "C" {
#endif
AMBR_API int __cdecl AveMediaBridge_ReaderGetMediaFactsV1(
    AMBR_PreparedInput*, char* utf8, uint32_t capacity, uint32_t* requiredBytes);
#ifdef __cplusplus
}
#endif
