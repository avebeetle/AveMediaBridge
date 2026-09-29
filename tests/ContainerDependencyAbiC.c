#include "AveMediaBridge/AveMediaBridgeContainerApi.h"
#include <stddef.h>
_Static_assert(sizeof(AMBI_SourceV1) == 72, "stable source layout");
int container_abi_c(void) {
    int (__cdecl *fn)(const AMBI_SourceV1*, char*, uint32_t, uint32_t*) =
        AveMediaBridge_ClassifyContainerV1;
    return fn(NULL, NULL, 0, NULL) == AMBR_INVALID_ARGUMENT;
}
