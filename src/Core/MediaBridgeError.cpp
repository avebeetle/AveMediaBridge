#include "MediaBridgeError.hpp"

extern "C" {
#include <libavutil/error.h>
}

namespace AveMediaBridge {

std::string ffErrorString(int err) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buffer, sizeof(buffer));
    return std::string(buffer);
}

}  // namespace AveMediaBridge
