#pragma once

#include "../Probe/Mp4AudioSampleTable.hpp"
#include <algorithm>

namespace AveMediaBridge::Decode {

// Read only the proved native PCM edit on the demuxer's already open handle.
// The caller retains the same format decoder, output writer and cancel path.
class MovPcmEditReader {
public:
    MovPcmEditReader(const Probe::MovPcmEdit& edit, int stream) : edit_(edit), stream_(stream) {}

    int read(AVIOContext* io, AVPacket* packet) {
        if (range_ == edit_.ranges.size())
            return AVERROR_EOF;
        const auto& range = edit_.ranges[range_];
        const auto frames = std::min<std::uint64_t>(range.frames - used_, 16384);
        const auto offset = static_cast<std::int64_t>(range.offset + used_ * edit_.bytesPerFrame);
        if (avio_tell(io) != offset && avio_seek(io, offset, SEEK_SET) != offset)
            return AVERROR(EIO);
        const int bytes = static_cast<int>(frames * edit_.bytesPerFrame);
        const int allocated = av_new_packet(packet, bytes);
        if (allocated < 0)
            return allocated;
        const int actual = avio_read(io, packet->data, bytes);
        if (actual != bytes) {
            av_packet_unref(packet);
            return actual < 0 && actual != AVERROR_EOF ? actual : AVERROR(EIO);
        }
        packet->pts = packet->dts = emitted_;
        packet->duration = frames;
        packet->pos = offset;
        packet->stream_index = stream_;
        packet->flags |= AV_PKT_FLAG_KEY;
        emitted_ += frames;
        used_ += frames;
        if (used_ == range.frames) {
            ++range_;
            used_ = 0;
        }
        return 0;
    }

private:
    const Probe::MovPcmEdit& edit_;
    int stream_;
    std::size_t range_ = 0;
    std::uint64_t used_ = 0;
    std::int64_t emitted_ = 0;
};

} // namespace AveMediaBridge::Decode
