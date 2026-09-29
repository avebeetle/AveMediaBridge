#pragma once
#include "AveMediaBridgeReaderApi.h"

#define AMBC_BUFFER_TOO_SMALL 8
#define AMBC_MAX_JSON_BYTES 65536u

#ifdef __cplusplus
extern "C" {
#endif
/* Synchronous stable-source dependency proof. Descriptor copied; callbacks
   borrowed only until return. Both calls must use the same immutable source.
   Query with null/0; requiredBytes includes NUL. No partial output on failure.
   Unknown/externalDependencies are successful reports, not API failures. */
AMBR_API int __cdecl AveMediaBridge_ClassifyContainerV1(
    const AMBI_SourceV1* source, char* utf8, uint32_t capacity, uint32_t* requiredBytes);
#ifdef __cplusplus
}
#endif
