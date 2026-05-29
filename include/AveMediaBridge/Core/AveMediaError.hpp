#pragma once

#include <string>

namespace AveMediaBridge {

enum class AveMediaErrorCode {
    None = 0,
    OpenInputFailed,
    StreamInfoFailed,
    NoAudioStream,
    DecoderUnavailable,
    DecodeFailed,
    ResampleFailed,
    InvalidAudioBuffer
};

struct AveMediaError {
    AveMediaErrorCode code = AveMediaErrorCode::None;
    std::string message;

    bool ok() const {
        return code == AveMediaErrorCode::None;
    }
};

}  // namespace AveMediaBridge
