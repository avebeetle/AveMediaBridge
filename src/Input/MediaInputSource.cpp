#include "Input/MediaInputSource.hpp"

namespace AveMediaBridge::Input {
MediaInputSource MediaInputSource::fromPath(std::string path) {
    MediaInputSource result;
    result.label_ = std::move(path);
    return result;
}

MediaInputSource MediaInputSource::fromStable(const AMBI_SourceV1& source,
    std::string label) {
    MediaInputSource result;
    result.stable_ = true;
    result.source_ = source;
    result.label_ = std::move(label);
    return result;
}

bool MediaInputSource::isStable() const noexcept { return stable_; }
const std::string& MediaInputSource::displayLabel() const noexcept { return label_; }
const std::string* MediaInputSource::legacyPath() const noexcept {
    return stable_ ? nullptr : &label_;
}
const AMBI_SourceV1* MediaInputSource::stableSource() const noexcept {
    return stable_ ? &source_ : nullptr;
}
}
