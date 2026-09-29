#include "AveMediaBridge/AveMediaBridgeMediaFactsApi.h"
#include <stddef.h>
#include <stdint.h>

typedef int (__cdecl *AMBM_ExpectedGet)(AMBR_PreparedInput*, char*, uint32_t, uint32_t*);
_Static_assert(AMBM_BUFFER_TOO_SMALL == 8, "media facts status");
_Static_assert(AMBM_MAX_JSON_BYTES == 65536u, "media facts cap");
#if defined(__GNUC__) || defined(__clang__)
_Static_assert(__builtin_types_compatible_p(__typeof__(&AveMediaBridge_ReaderGetMediaFactsV1), AMBM_ExpectedGet), "media facts C signature");
#else
_Static_assert(_Generic(&AveMediaBridge_ReaderGetMediaFactsV1,
    AMBM_ExpectedGet: 1, default: 0), "media facts C signature and calling convention");
#endif
_Static_assert(sizeof(AMBM_ExpectedGet) == sizeof(&AveMediaBridge_ReaderGetMediaFactsV1),
    "media facts C pointer size");
