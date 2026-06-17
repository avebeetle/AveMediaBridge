#include "FfmpegStreamSelection.hpp"

namespace AveMediaBridge::Ffmpeg {

bool isAudioStream(const AVStream* stream) noexcept {
    return stream &&
        stream->codecpar &&
        stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
}

int findFirstAudioStream(const AVFormatContext* formatContext) noexcept {
    if (!formatContext) {
        return -1;
    }

    for (unsigned int i = 0; i < formatContext->nb_streams; ++i) {
        if (isAudioStream(formatContext->streams[i])) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

AudioStreamSelection selectBestAudioStreamStrict(AVFormatContext* formatContext) noexcept {
    AudioStreamSelection result;
    if (!formatContext) {
        result.bestStreamResult = AVERROR(EINVAL);
        return result;
    }

    const AVCodec* decoder = nullptr;
    const int streamIndex =
        av_find_best_stream(formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    result.bestStreamResult = streamIndex;
    if (streamIndex >= 0) {
        result.streamIndex = streamIndex;
        result.decoder = decoder;
    }
    return result;
}

AudioStreamSelection selectBestAudioStreamWithFirstAudioFallback(
    AVFormatContext* formatContext) noexcept {
    AudioStreamSelection result = selectBestAudioStreamStrict(formatContext);
    if (result.streamIndex >= 0 || !formatContext) {
        return result;
    }

    const int fallbackStreamIndex = findFirstAudioStream(formatContext);
    if (fallbackStreamIndex < 0) {
        return result;
    }

    result.streamIndex = fallbackStreamIndex;
    result.usedFirstAudioFallback = true;
    if (fallbackStreamIndex < static_cast<int>(formatContext->nb_streams)) {
        const AVStream* stream = formatContext->streams[fallbackStreamIndex];
        if (stream && stream->codecpar) {
            result.decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        }
    }
    return result;
}

}  // namespace AveMediaBridge::Ffmpeg

