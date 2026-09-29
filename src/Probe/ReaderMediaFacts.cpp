#include "ReaderMediaFacts.hpp"
#include "../Utils/JsonUtils.hpp"
#include <algorithm>
#include <limits>
#include <string_view>

namespace AveMediaBridge::Probe {
namespace {
bool isStillImage(AVCodecID codec) {
    const AVCodecDescriptor* descriptor = avcodec_descriptor_get(codec);
    if (descriptor && descriptor->mime_types) {
        for (const char* const* mime = descriptor->mime_types; *mime; ++mime)
            if (std::string_view(*mime).substr(0, 6) == "image/") return true;
    }
    switch (codec) {
    case AV_CODEC_ID_MJPEG: case AV_CODEC_ID_PNG: case AV_CODEC_ID_BMP:
    case AV_CODEC_ID_GIF: case AV_CODEC_ID_TIFF: case AV_CODEC_ID_WEBP:
        return true;
    // Dedicated still-image formats in FFmpeg 7.1's image2 map or codec
    // descriptors which lack image/ MIME metadata. Do not use INTRA_ONLY:
    // that also describes ordinary video codecs and raw video.
    case AV_CODEC_ID_PPM: case AV_CODEC_ID_PBM: case AV_CODEC_ID_PGM:
    case AV_CODEC_ID_PGMYUV: case AV_CODEC_ID_PFM: case AV_CODEC_ID_PHM:
    case AV_CODEC_ID_LJPEG: case AV_CODEC_ID_JPEGLS:
    case AV_CODEC_ID_ALIAS_PIX: case AV_CODEC_ID_DDS: case AV_CODEC_ID_TARGA:
    case AV_CODEC_ID_SGI: case AV_CODEC_ID_PTX: case AV_CODEC_ID_PHOTOCD:
    case AV_CODEC_ID_QDRAW: case AV_CODEC_ID_PICTOR: case AV_CODEC_ID_SUNRAST:
    case AV_CODEC_ID_DPX: case AV_CODEC_ID_EXR: case AV_CODEC_ID_XFACE:
    case AV_CODEC_ID_GEM: case AV_CODEC_ID_VBN: case AV_CODEC_ID_QOI:
    case AV_CODEC_ID_RADIANCE_HDR: case AV_CODEC_ID_WBMP:
    case AV_CODEC_ID_TXD: case AV_CODEC_ID_BRENDER_PIX:
    case AV_CODEC_ID_PSD: case AV_CODEC_ID_FITS:
        return true;
    default: return false;
    }
}
bool timed(const AVStream* stream) {
    return stream->time_base.num > 0 && stream->time_base.den > 0 &&
        stream->duration > 0 && stream->duration != AV_NOPTS_VALUE &&
        ((stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) ||
         (stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0));
}
std::string hex(const std::uint8_t* data, std::size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15];
    }
    return out;
}
void appendQuoted(std::string& out, const std::string& value) { out += Utils::jsonString(value); }
}
ReaderMediaFacts captureReaderMediaFacts(const AVFormatContext* context, const Input::SelectedAudioBinding& binding) {
    ReaderMediaFacts facts;
    facts.selectedAudio = binding;
    if (!context) return facts;
    bool uncertain = false;
    for (unsigned i = 0; i < context->nb_streams; ++i) {
        const AVStream* stream = context->streams[i];
        if (!stream || !stream->codecpar) { uncertain = true; continue; }
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
            (stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) continue;
        if (isStillImage(stream->codecpar->codec_id) && !timed(stream)) continue;
        if (stream->codecpar->codec_id == AV_CODEC_ID_NONE ||
            stream->codecpar->width <= 0 || stream->codecpar->height <= 0 || !timed(stream)) {
            uncertain = true;
            continue;
        }
        if (facts.timedVideoCount != std::numeric_limits<unsigned>::max())
            ++facts.timedVideoCount;
        if (facts.timedVideoCount == 1) {
            facts.videoStreamIndex = static_cast<int>(i);
            facts.videoTrackId = stream->id;
            facts.videoCodecId = static_cast<int>(stream->codecpar->codec_id);
            facts.videoCodec = avcodec_get_name(stream->codecpar->codec_id);
        } else {
            facts.videoStreamIndex = -1;
            facts.videoTrackId = -1;
            facts.videoCodecId = 0;
            facts.videoCodec.clear();
        }
    }
    if (uncertain) {
        facts.videoClassification = "unknown";
        facts.videoReason = "uncertainTimingOrCodec";
        facts.videoStreamIndex = -1;
        facts.videoTrackId = -1;
        facts.videoCodecId = 0;
        facts.videoCodec.clear();
    } else if (facts.timedVideoCount == 0) {
        facts.videoClassification = "none";
        facts.videoReason = "noTimedVideo";
    } else if (facts.timedVideoCount == 1) {
        facts.videoClassification = "singleTimed";
        facts.videoReason.clear();
    } else {
        facts.videoClassification = "multipleTimed";
        facts.videoReason = "multipleTimedStreams";
    }
    return facts;
}
std::string encodeReaderMediaFacts(const ReaderMediaFacts& facts) {
    const auto& a = facts.selectedAudio;
    if (a.extradata.size() > 30000 || a.demuxerName.size() > 128 ||
        a.channelLayout.size() > 256) return {};
    std::string out = "{\"version\":1,\"sourceToken\":\"" + hex(a.sourceToken.data(), a.sourceToken.size()) +
        "\",\"byteSize\":\"" + std::to_string(a.byteSize) + "\",\"demuxer\":";
    appendQuoted(out, a.demuxerName);
    out += ",\"selectedAudio\":{\"streamIndex\":" + std::to_string(a.streamIndex) +
        ",\"trackId\":" + std::to_string(a.trackId) + ",\"codecId\":" +
        std::to_string(a.codecId) + ",\"codec\":";
    appendQuoted(out, avcodec_get_name(static_cast<AVCodecID>(a.codecId)));
    out += ",\"nativeSampleRate\":" + std::to_string(a.sampleRate) +
        ",\"nativeChannels\":" + std::to_string(a.channels) + ",\"channelLayout\":";
    appendQuoted(out, a.channelLayout);
    out += ",\"timeBase\":{\"numerator\":" + std::to_string(a.timeBaseNumerator) +
        ",\"denominator\":" + std::to_string(a.timeBaseDenominator) +
        "},\"extradataHex\":\"" + hex(a.extradata.data(), a.extradata.size()) + "\"}";
    out += ",\"video\":{\"classification\":";
    appendQuoted(out, facts.videoClassification);
    out += ",\"timedVideoCount\":" + std::to_string(facts.timedVideoCount) +
        ",\"reason\":";
    appendQuoted(out, facts.videoReason);
    if (facts.videoClassification == "singleTimed") {
        out += ",\"streamIndex\":" + std::to_string(facts.videoStreamIndex) +
            ",\"trackId\":" + std::to_string(facts.videoTrackId) +
            ",\"codecId\":" + std::to_string(facts.videoCodecId) + ",\"codec\":";
        appendQuoted(out, facts.videoCodec);
    }
    out += "},\"audioPresentationMap\":{\"status\":\"unavailable\",\"reason\":\"notQualified\"},"
        "\"dependency\":{\"status\":\"unknown\",\"policy\":\"notClassified\",\"version\":1}}";
    return out;
}
}
