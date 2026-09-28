#pragma once

#include "AveMediaBridge/AveMediaBridgeInputApi.h"

#include <memory>

struct AVFormatContext;
struct AVIOContext;
struct AVInputFormat;
struct AVDictionary;

namespace AveMediaBridge::Input {
// Copies the descriptor but borrows source.user. The caller must keep its
// adapter/user context alive until all calls are joined and this object is
// destroyed.
class StableAvioInput final {
public:
    static int create(const AMBI_SourceV1& source,
        std::unique_ptr<StableAvioInput>& out) noexcept;
    ~StableAvioInput() noexcept;

    StableAvioInput(const StableAvioInput&) = delete;
    StableAvioInput& operator=(const StableAvioInput&) = delete;
    StableAvioInput(StableAvioInput&&) = delete;
    StableAvioInput& operator=(StableAvioInput&&) = delete;

    int open(const AVInputFormat* forcedFormat) noexcept;
    AVIOContext* io() const noexcept { return io_; }
    AVFormatContext* format() const noexcept { return format_; }

private:
    explicit StableAvioInput(const AMBI_SourceV1& source) noexcept : source_(source) {}
    static int readPacket(void* opaque, uint8_t* destination, int requested) noexcept;
    static int64_t seekPacket(void* opaque, int64_t offset, int whence) noexcept;
    static int denySecondary(AVFormatContext*, AVIOContext**, const char*, int,
        AVDictionary**) noexcept;

    AMBI_SourceV1 source_{};
    AVIOContext* io_ = nullptr;
    AVFormatContext* format_ = nullptr;
    int64_t cursor_ = 0;
    int terminalReadError_ = 0;
    bool opened_ = false;
};
}
