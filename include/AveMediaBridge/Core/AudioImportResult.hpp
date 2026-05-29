#pragma once

#include <string>

#include "AveMediaBridge/Core/AudioBufferF32.hpp"
#include "AveMediaBridge/Core/AudioStats.hpp"
#include "AveMediaBridge/Core/MediaInfo.hpp"

namespace AveMediaBridge {

struct AudioImportResult {
    bool ok = false;
    std::string error;
    AudioBufferF32 audio;
    SourceMediaInfo source;
    MediaProbeDetails probe;
    SelectedAudioStreamInfo selectedAudio;
    DecodeReport decode;
    AudioStats stats;

    bool hasUsableAudio() const {
        return ok && audio.isConsistent() && !audio.empty();
    }
};

}  // namespace AveMediaBridge
