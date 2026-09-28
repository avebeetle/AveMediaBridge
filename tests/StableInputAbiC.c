#include "AveMediaBridge/AveMediaBridgeInputApi.h"
#include <stddef.h>

typedef char ambi_size[(sizeof(AMBI_SourceV1) == 72) ? 1 : -1];
typedef char ambi_align[(__alignof(AMBI_SourceV1) == 8) ? 1 : -1];
typedef char ambi_byte_size[(offsetof(AMBI_SourceV1, byteSize) == 8) ? 1 : -1];
typedef char ambi_token[(offsetof(AMBI_SourceV1, sourceToken) == 16) ? 1 : -1];
typedef char ambi_user[(offsetof(AMBI_SourceV1, user) == 32) ? 1 : -1];
typedef char ambi_read[(offsetof(AMBI_SourceV1, readAt) == 40) ? 1 : -1];
typedef char ambi_cancel[(offsetof(AMBI_SourceV1, checkCancel) == 48) ? 1 : -1];
typedef char ambi_reserved[(offsetof(AMBI_SourceV1, reserved) == 56) ? 1 : -1];

int ambi_c_header_consumer(void) {
    AMBI_SourceV1 source = {0};
    return (int)source.abiVersion;
}
