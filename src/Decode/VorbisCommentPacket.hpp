#pragma once

#include "Decode/VorbisInput.hpp"
#include <algorithm>
#include <cstring>

namespace AveMediaBridge::Vorbis::detail {

// Comments contain length-prefixed text. Validate its envelope while pages are
// read and let the packet decoder see a minimal comment instead of retaining text.
class CommentPacket {
    enum class Field { prefix, vendorLength, vendor, count, textLength, text, framing, padding };
    Field field_ = Field::prefix;
    unsigned offset_ = 0;
    uint32_t word_ = 0, remaining_ = 0, fields_ = 0;

    void nextText() {
        field_ = fields_ ? Field::textLength : Field::framing;
    }

public:
    void append(const uint8_t* bytes, size_t count) {
        static constexpr uint8_t prefix[] = {3, 'v', 'o', 'r', 'b', 'i', 's'};
        while (count) {
            if (field_ == Field::padding) return;
            if (field_ == Field::prefix) {
                const auto n = std::min(count, sizeof(prefix) - offset_);
                require(!std::memcmp(bytes, prefix + offset_, n), "Vorbis comment signature");
                bytes += n; count -= n; offset_ += static_cast<unsigned>(n);
                if (offset_ == sizeof(prefix)) { field_ = Field::vendorLength; offset_ = 0; }
            } else if (field_ == Field::vendor || field_ == Field::text) {
                const auto n = static_cast<uint32_t>(std::min<size_t>(count, remaining_));
                remaining_ -= n;
                bytes += n; count -= n;
                if (!remaining_) {
                    if (field_ == Field::vendor) field_ = Field::count;
                    else nextText();
                }
            } else if (field_ == Field::framing) {
                require((*bytes++ & 1) != 0, "Vorbis comment framing");
                --count; field_ = Field::padding;
            } else {
                word_ |= uint32_t(*bytes++) << (offset_++ * 8);
                --count;
                if (offset_ != 4) continue;
                const uint32_t value = word_;
                word_ = 0; offset_ = 0;
                if (field_ == Field::count) { fields_ = value; nextText(); }
                else {
                    remaining_ = value;
                    if (field_ == Field::vendorLength) field_ = value ? Field::vendor : Field::count;
                    else { --fields_; if (value) field_ = Field::text; else nextText(); }
                }
            }
        }
    }
    bool complete() const { return field_ == Field::padding; }
};
} // namespace AveMediaBridge::Vorbis::detail
