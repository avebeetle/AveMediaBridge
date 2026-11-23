#pragma once

#include "MediaInputSource.hpp"
#include "../Ffmpeg/FfmpegHeaders.hpp"

#include <cstdint>
#include <cstring>
#include <mutex>

namespace AveMediaBridge::Input {

// Synchronously borrow the demuxer's already-open local input. Both the length
// proof and decoding must see this handle, even if its pathname is replaced.
class DemuxByteSource final {
public:
    explicit DemuxByteSource(AVIOContext* io) : io_(io) {
        if (!io_ || !(io_->seekable & AVIO_SEEKABLE_NORMAL)) return;
        position_ = avio_tell(io_);
        const auto size = avio_size(io_);
        if (position_ < 0 || size <= 0) return;
        descriptor_.structSize = sizeof descriptor_;
        descriptor_.abiVersion = AMBI_ABI_VERSION;
        descriptor_.byteSize = static_cast<std::uint64_t>(size);
        descriptor_.user = this;
        descriptor_.readAt = &readAt;
        descriptor_.checkCancel = [](void*) -> AMBI_Status { return AMBI_OK; };
        const auto identity = reinterpret_cast<std::uintptr_t>(this);
        std::memcpy(descriptor_.sourceToken, &identity, sizeof identity);
        originalDirect_ = io_->direct;
        // Small sparse reads must not trigger FFmpeg read-ahead/short seeks.
        io_->direct = 1;
    }

    DemuxByteSource(const DemuxByteSource&) = delete;
    DemuxByteSource& operator=(const DemuxByteSource&) = delete;
    ~DemuxByteSource() { restore(); }
    bool valid() const { return descriptor_.byteSize != 0; }
    MediaInputSource source(const std::string& label) const {
        return MediaInputSource::fromStable(descriptor_, label);
    }
    bool restore() {
        if (!valid()) return false;
        const bool restored = avio_seek(io_, position_, SEEK_SET) == position_;
        io_->direct = originalDirect_;
        descriptor_.byteSize = 0;
        return restored;
    }

private:
    static AMBI_Status __cdecl readAt(void* user, std::uint64_t offset, void* buffer,
                                     std::uint32_t count, std::uint32_t* bytesRead) {
        auto& input = *static_cast<DemuxByteSource*>(user);
        *bytesRead = 0;
        if (!count) return AMBI_OK;
        std::lock_guard<std::mutex> lock(input.mutex_);
        const auto pos = static_cast<std::int64_t>(offset);
        if (avio_tell(input.io_) != pos && avio_seek(input.io_, pos, SEEK_SET) != pos)
            return AMBI_IO_ERROR;
        const int n = avio_read(input.io_, static_cast<unsigned char*>(buffer), static_cast<int>(count));
        if (n < 0) return AMBI_IO_ERROR;
        *bytesRead = static_cast<std::uint32_t>(n);
        return *bytesRead == count ? AMBI_OK : AMBI_IO_ERROR;
    }

    AVIOContext* io_;
    std::int64_t position_ = -1;
    int originalDirect_ = 0;
    AMBI_SourceV1 descriptor_{};
    std::mutex mutex_;
};
} // namespace AveMediaBridge::Input
