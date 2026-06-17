#include "AudioDecodeHelpers.hpp"

#include <algorithm>

namespace AveMediaBridge::Decode {

int copyDecodedFrameLayout(
    AVChannelLayout* dst,
    const AVFrame* frame,
    const AVCodecContext* decoder) {
    if (frame->ch_layout.nb_channels > 0 && av_channel_layout_check(&frame->ch_layout)) {
        return av_channel_layout_copy(dst, &frame->ch_layout);
    }

    if (decoder->ch_layout.nb_channels > 0 && av_channel_layout_check(&decoder->ch_layout)) {
        return av_channel_layout_copy(dst, &decoder->ch_layout);
    }

    const int channels = (std::max)(frame->ch_layout.nb_channels, decoder->ch_layout.nb_channels);
    if (channels <= 0) {
        return AVERROR(EINVAL);
    }

    av_channel_layout_default(dst, channels);
    return 0;
}

}  // namespace AveMediaBridge::Decode
