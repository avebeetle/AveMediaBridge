#pragma once

#include "AveMediaBridge/AveMediaBridgeInputApi.h"

#include <string>

namespace AveMediaBridge::Input {
class MediaInputSource final {
public:
    static MediaInputSource fromPath(std::string path);
    static MediaInputSource fromStable(const AMBI_SourceV1& source, std::string label);

    bool isStable() const noexcept;
    const std::string& displayLabel() const noexcept;
    const std::string* legacyPath() const noexcept;
    const AMBI_SourceV1* stableSource() const noexcept;

private:
    bool stable_ = false;
    std::string label_;
    AMBI_SourceV1 source_{};
};
}
