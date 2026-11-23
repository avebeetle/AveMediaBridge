#include "MatroskaVorbisTrack.hpp"
#include "EbmlSequentialReader.hpp"
#include "../Ffmpeg/FfmpegDeleters.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <string_view>

extern "C" {
#include <libavcodec/vorbis_parser.h>
}

namespace AveMediaBridge::Probe {
namespace {

constexpr std::int64_t maxFileBytes = 1 << 20;
constexpr int maxPackets = 16384;

bool hasCompleteSegment(AVIOContext* io, std::int64_t size) {
    const auto saved = avio_tell(io);
    if (saved < 0 || avio_seek(io, 0, SEEK_SET) != 0) return false;
    std::array<std::uint8_t, 256> head{};
    const int available = avio_read(io, head.data(), static_cast<int>(head.size()));
    if (avio_seek(io, saved, SEEK_SET) != saved || available < 12 ||
        std::memcmp(head.data(), "\x1a\x45\xdf\xa3", 4) != 0) return false;
    using MatroskaAacDetail::readMemoryVint;
    const auto ebml = readMemoryVint(head.data(), available, 4, false);
    if (!ebml.valid || ebml.unknown || ebml.value > head.size()) return false;
    const auto segment = 4 + ebml.length + static_cast<std::size_t>(ebml.value);
    if (segment + 4 >= static_cast<std::size_t>(available) ||
        std::memcmp(head.data() + segment, "\x18\x53\x80\x67", 4) != 0) return false;
    const auto length = readMemoryVint(head.data(), available, segment + 4, false);
    return length.valid && !length.unknown &&
        segment + 4 + length.length + length.value == static_cast<std::uint64_t>(size);
}

// The Matroska muxer's DURATION tag is a presentation *end*, in nanoseconds.
// Only use a zero-origin, selected-track tag for a provisional width.
std::int64_t selectedTagFrames(const AVFormatContext* context, const AVStream* audio) {
    if (audio->start_time != 0 || context->duration <= 0) return 0;
    const auto* entry = av_dict_get(audio->metadata, "DURATION", nullptr, 0);
    if (!entry || !entry->value) return 0;
    const std::string_view tag(entry->value);
    if (tag.size() != 18 || tag[2] != ':' || tag[5] != ':' || tag[8] != '.') return 0;
    const auto number = [&](int begin, int end) {
        int value = 0;
        for (int i = begin; i < end; ++i) {
            if (tag[i] < '0' || tag[i] > '9') return -1;
            value = value * 10 + tag[i] - '0';
        }
        return value;
    };
    const int h = number(0, 2), m = number(3, 5), s = number(6, 8), ns = number(9, 18);
    if (h < 0 || m < 0 || m >= 60 || s < 0 || s >= 60 || ns < 0) return 0;
    const std::int64_t totalNs = ((h * 60 + m) * 60 + s) * INT64_C(1000000000) + ns;
    if (totalNs / 1000 > context->duration) return 0;
    return av_rescale(totalNs, audio->codecpar->sample_rate, INT64_C(1000000000));
}

struct ParserFree { void operator()(AVVorbisParseContext* parser) const { av_vorbis_parse_free(&parser); } };

std::int64_t boundedPacketFrames(AVFormatContext* context, const AVStream* audio) {
    const auto* codec = audio->codecpar;
    auto* io = context->pb;
    if (!io || !(io->seekable & AVIO_SEEKABLE_NORMAL) || codec->ch_layout.nb_channels > 2 ||
        codec->initial_padding || codec->trailing_padding || codec->nb_coded_side_data || !codec->extradata ||
        codec->extradata_size <= 0 || codec->extradata_size > 65536 ||
        audio->time_base.num != 1 || audio->time_base.den != 1000 || audio->start_time != 0) return 0;
    const auto size = avio_size(io);
    if (size <= 0 || size > maxFileBytes || !hasCompleteSegment(io, size)) return 0;
    const auto bytesAtStart = io->bytes_read;
    std::unique_ptr<AVVorbisParseContext, ParserFree> parser(
        av_vorbis_parse_init(codec->extradata, codec->extradata_size));
    Ffmpeg::UniqueAVPacket packet(av_packet_alloc());
    if (!parser || !packet) return 0;
    const AVRational sampleTime{1, codec->sample_rate};
    std::int64_t encodedFrames = 0, primingFrames = 0;
    for (int i = 0; i < maxPackets; ++i) {
        const int rc = av_read_frame(context, packet.get());
        if (io->bytes_read - bytesAtStart > 2 * maxFileBytes) return 0;
        if (rc < 0) {
            return rc == AVERROR_EOF && !io->error && avio_size(io) == size
                ? encodedFrames - primingFrames : 0;
        }
        if (packet->stream_index == audio->index) {
            if (!packet->data || packet->size <= 0 || packet->size > 65536 ||
                (packet->data[0] & 1) || packet->side_data_elems ||
                (packet->flags & (AV_PKT_FLAG_CORRUPT | AV_PKT_FLAG_DISCARD)) ||
                packet->pts < 0 || (encodedFrames == 0 && packet->pts != 0) ||
                packet->dts != packet->pts) return 0;
            const auto expectedPts = av_rescale_q(encodedFrames, sampleTime, audio->time_base);
            // One Matroska tick covers nearest/floor timestamp rounding.
            if (packet->pts < expectedPts - 1 || packet->pts > expectedPts + 1) return 0;
            const int frames = av_vorbis_parse_frame(parser.get(), packet->data, packet->size);
            if (frames <= 0 || frames > 8192) return 0;
            if (encodedFrames == 0) primingFrames = frames;
            encodedFrames += frames;
            if (encodedFrames - primingFrames > codec->sample_rate * INT64_C(120)) return 0;
        }
        av_packet_unref(packet.get());
    }
    return 0;
}

} // namespace

std::optional<std::int64_t> estimateMatroskaVorbisTrack(const AVFormatContext* context, const AVStream* audio) {
    if (!context || !context->iformat || !context->iformat->name ||
        std::string_view(context->iformat->name) != "matroska,webm" || !audio || !audio->codecpar ||
        audio->codecpar->codec_id != AV_CODEC_ID_VORBIS || audio->codecpar->sample_rate < 8000 ||
        audio->codecpar->sample_rate > 192000 || audio->codecpar->ch_layout.nb_channels <= 0 ||
        audio->codecpar->ch_layout.nb_channels > 2 ||
        context->nb_streams != 2 || audio->index < 0 || audio->index > 1 ||
        context->streams[audio->index] != audio) return {};
    const auto* video = context->streams[1 - audio->index];
    if (!video || !video->codecpar || video->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
        (video->disposition & AV_DISPOSITION_ATTACHED_PIC)) return {};
    return selectedTagFrames(context, audio);
}

MatroskaVorbisTrackExtent probeMatroskaVorbisTrack(AVFormatContext* context, const AVStream* audio) {
    const auto estimate = estimateMatroskaVorbisTrack(context, audio);
    if (!estimate.has_value()) return {};
    return {boundedPacketFrames(context, audio), *estimate};
}

} // namespace AveMediaBridge::Probe
