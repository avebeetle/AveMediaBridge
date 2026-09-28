#include "Input/SelectedAudioBinding.hpp"

#include "Input/ReaderInputError.hpp"

#include <algorithm>
#include <cstring>

namespace AveMediaBridge::Input {
SelectedAudioBinding bindSelectedAudio(
    const MediaInputSource& source, const AVFormatContext* context, int streamIndex) {
    const AMBI_SourceV1* stable = source.stableSource();
    if (!stable || !context || !context->iformat || !context->iformat->name ||
        streamIndex < 0 || streamIndex >= static_cast<int>(context->nb_streams) ||
        !context->streams[streamIndex] || !context->streams[streamIndex]->codecpar ||
        context->streams[streamIndex]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        throw ReaderInputError(ReaderInputFailure::InputFailed,
            "invalid selected audio binding inputs");
    }
    const AVStream* stream = context->streams[streamIndex];
    const AVCodecParameters* codec = stream->codecpar;
    if (codec->extradata_size < 0 ||
        (codec->extradata_size > 0 && !codec->extradata)) {
        throw ReaderInputError(ReaderInputFailure::InputFailed,
            "invalid selected audio extradata");
    }
    SelectedAudioBinding result;
    std::copy_n(stable->sourceToken, result.sourceToken.size(),
        result.sourceToken.begin());
    result.byteSize = stable->byteSize;
    result.demuxerName = context->iformat->name;
    result.streamIndex = streamIndex;
    result.trackId = stream->id;
    result.codecId = static_cast<int>(codec->codec_id);
    result.sampleRate = codec->sample_rate;
    result.channels = codec->ch_layout.nb_channels;
    char layout[128]{};
    if (codec->ch_layout.nb_channels > 0 &&
        av_channel_layout_check(&codec->ch_layout) &&
        av_channel_layout_describe(&codec->ch_layout, layout, sizeof layout) > 0)
        result.channelLayout = layout;
    result.timeBaseNumerator = stream->time_base.num;
    result.timeBaseDenominator = stream->time_base.den;
    if (codec->extradata_size > 0)
        result.extradata.assign(codec->extradata,
            codec->extradata + codec->extradata_size);
    return result;
}

bool matchesSelectedAudio(
    const SelectedAudioBinding& expected, const MediaInputSource& source,
    const AVFormatContext* context, int streamIndex) {
    try {
        const SelectedAudioBinding actual = bindSelectedAudio(source, context, streamIndex);
        return expected.sourceToken == actual.sourceToken &&
            expected.byteSize == actual.byteSize &&
            expected.demuxerName == actual.demuxerName &&
            expected.streamIndex == actual.streamIndex &&
            expected.trackId == actual.trackId &&
            expected.codecId == actual.codecId &&
            expected.sampleRate == actual.sampleRate &&
            expected.channels == actual.channels &&
            expected.channelLayout == actual.channelLayout &&
            expected.timeBaseNumerator == actual.timeBaseNumerator &&
            expected.timeBaseDenominator == actual.timeBaseDenominator &&
            expected.extradata == actual.extradata;
    } catch (const ReaderInputError&) {
        return false;
    }
}
}
