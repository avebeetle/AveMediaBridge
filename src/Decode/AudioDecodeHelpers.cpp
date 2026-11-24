#include "AudioDecodeHelpers.hpp"

#include <algorithm>
#include <cstring>

namespace AveMediaBridge::Decode {

void configureElementaryDecode(const AVFormatContext& input, AVCodecContext& decoder) {
    // Score 2 is the complete short-frame tie-break of the bundled raw demuxers.
    // Header agreement identifies the format; a decoder error invalidates that import.
    if (input.probe_score != 2 || !input.iformat) return;
    const char* name = input.iformat->name;
    if (std::strcmp(name, "aac") == 0 || std::strcmp(name, "ac3") == 0 ||
        std::strcmp(name, "eac3") == 0) {
        decoder.err_recognition |= AV_EF_EXPLODE;
    }
}

bool rejectDamagedPacket(const AVCodecContext& decoder, int error, std::string& message) {
    if (!(decoder.err_recognition & AV_EF_EXPLODE)) return false;
    char detail[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, detail, sizeof(detail));
    message = std::string("Failed to decode a complete elementary audio packet: ") + detail;
    return true;
}

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
