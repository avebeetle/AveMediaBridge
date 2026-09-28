#include "AveMediaBridge/AveMediaBridgeExportApi.h"
#include <stddef.h>

typedef char ambe_input_size[(sizeof(AMBE_InputV1) == 48) ? 1 : -1];
typedef char ambe_input_frame_offset[(offsetof(AMBE_InputV1, expectedFrames) == 24) ? 1 : -1];
typedef char ambe_capability_size[(sizeof(AMBE_CapabilitiesV1) == 32) ? 1 : -1];
typedef char ambe_capability_bit_offset[(offsetof(AMBE_CapabilitiesV1, profileBits) == 8) ? 1 : -1];
typedef char ambe_result_size[(sizeof(AMBE_ResultV1) == 32) ? 1 : -1];
typedef char ambe_result_frame_offset[(offsetof(AMBE_ResultV1, acceptedFrames) == 8) ? 1 : -1];

int ambe_c_header_consumer(void) {
    AMBE_InputV1 input = {0};
    return (int)input.abiVersion;
}
