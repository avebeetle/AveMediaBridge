#pragma once

#include "FfmpegHeaders.hpp"

#include <memory>

namespace AveMediaBridge::Ffmpeg {

struct InputFormatContextDeleter {
    void operator()(AVFormatContext* context) const noexcept {
        if (!context) {
            return;
        }
        AVFormatContext* mutableContext = context;
        avformat_close_input(&mutableContext);
    }
};

struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const noexcept {
        if (!context) {
            return;
        }
        AVCodecContext* mutableContext = context;
        avcodec_free_context(&mutableContext);
    }
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const noexcept {
        if (!packet) {
            return;
        }
        AVPacket* mutablePacket = packet;
        av_packet_free(&mutablePacket);
    }
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const noexcept {
        if (!frame) {
            return;
        }
        AVFrame* mutableFrame = frame;
        av_frame_free(&mutableFrame);
    }
};

struct SwrContextDeleter {
    void operator()(SwrContext* context) const noexcept {
        if (!context) {
            return;
        }
        SwrContext* mutableContext = context;
        swr_free(&mutableContext);
    }
};

using UniqueAVFormatContext = std::unique_ptr<AVFormatContext, InputFormatContextDeleter>;
using UniqueAVPacket = std::unique_ptr<AVPacket, PacketDeleter>;

inline void closeInput(AVFormatContext*& context) noexcept {
    InputFormatContextDeleter{}(context);
    context = nullptr;
}

inline void freeCodecContext(AVCodecContext*& context) noexcept {
    CodecContextDeleter{}(context);
    context = nullptr;
}

inline void freePacket(AVPacket*& packet) noexcept {
    PacketDeleter{}(packet);
    packet = nullptr;
}

inline void freeFrame(AVFrame*& frame) noexcept {
    FrameDeleter{}(frame);
    frame = nullptr;
}

inline void freeSwr(SwrContext*& context) noexcept {
    SwrContextDeleter{}(context);
    context = nullptr;
}

}  // namespace AveMediaBridge::Ffmpeg
