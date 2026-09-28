#ifndef AVEMEDIABRIDGE_INPUT_API_H
#define AVEMEDIABRIDGE_INPUT_API_H

#include <stdint.h>

#if !defined(_WIN64)
#error AveMediaBridge input ABI v1 supports Windows x64 only
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t AMBI_Status;

#define AMBI_ABI_VERSION 1u
#define AMBI_MAX_READ_BYTES 4194304u
#define AMBI_OK 0u
#define AMBI_EOF 1u
#define AMBI_CANCELED 2u
#define AMBI_INVALID_ARGUMENT 3u
#define AMBI_IO_ERROR 4u

typedef AMBI_Status (__cdecl *AMBI_ReadAtV1)(void* user, uint64_t offset,
    void* destination, uint32_t requestedBytes, uint32_t* bytesRead);
typedef AMBI_Status (__cdecl *AMBI_CheckCancelV1)(void* user);

#pragma pack(push, 8)
typedef struct AMBI_SourceV1 {
    uint32_t structSize, abiVersion;
    uint64_t byteSize;
    uint8_t sourceToken[16];
    void* user;
    AMBI_ReadAtV1 readAt;
    AMBI_CheckCancelV1 checkCancel;
    uint32_t reserved[4];
} AMBI_SourceV1;
#pragma pack(pop)

/* sourceToken is a nonzero opaque lifetime identity, shared by adapters of
   the same pin; it is not a digest or persistent file identity. Consumers
   copy the descriptor but borrow user until work is joined and destroyed.
   Callbacks support concurrent consumers, never retain destination, and do
   not re-enter the same reader. Cancellation checks are bounded/nonblocking.
   user may be null; readAt and checkCancel are required. destination may be
   null only for zero bytes; bytesRead is required and starts at zero. */

#ifdef __cplusplus
}
#endif
#endif
