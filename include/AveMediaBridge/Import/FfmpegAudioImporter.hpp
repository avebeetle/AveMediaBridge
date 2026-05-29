#pragma once

#include <string>

#include "AveMediaBridge/Core/AudioImportResult.hpp"

namespace AveMediaBridge {

class FfmpegAudioImporter {
public:
    AudioImportResult importFile(const std::string& path);
};

}  // namespace AveMediaBridge
