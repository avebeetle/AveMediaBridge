#include "Mp4AudioSampleTable.hpp"
#include "../Input/DemuxByteSource.hpp"
#include "../Input/ReaderInputError.hpp"
#include "../Input/StableInputContract.hpp"

extern "C" {
#include <libavutil/intreadwrite.h>
}
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

namespace AveMediaBridge::Probe {
namespace {
using U64 = std::uint64_t;
constexpr U64 readBudget = 4 * 1024 * 1024;
constexpr unsigned maxEntries = 1U << 20;

// Unsupported metadata takes the usual packet-check path. Cancellation and
// failures of the borrowed source are propagated separately.
struct Unsupported {
    const char* reason;
};
void require(bool condition, const char* reason) {
    if (!condition)
        throw Unsupported{reason};
}
void pollCancel(const std::function<bool()>& canceled) {
    if (canceled && canceled())
        throw Input::ReaderInputError(Input::ReaderInputFailure::Canceled,
                                      "MP4 audio index inspection canceled");
}

struct View {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    explicit operator bool() const { return data != nullptr; }
    View sub(std::size_t offset, std::size_t length) const {
        require(offset <= size && length <= size - offset, "box_bounds");
        return {data + offset, length};
    }
    View tail(std::size_t offset) const {
        require(offset <= size, "box_bounds");
        return sub(offset, size - offset);
    }
    unsigned byte(std::size_t p) const { return *sub(p, 1).data; }
    unsigned u16(std::size_t p) const { return AV_RB16(sub(p, 2).data); }
    unsigned u32(std::size_t p) const { return AV_RB32(sub(p, 4).data); }
    U64 u64(std::size_t p) const { return AV_RB64(sub(p, 8).data); }
    bool equals(const void* other, std::size_t length) const {
        return size == length && !std::memcmp(data, other, length);
    }
};

struct Box {
    std::string_view tag;
    View contents;
};
class Children {
public:
    explicit Children(View view) {
        while (view.size) {
            require(boxes_.size() < 256, "box_count");
            const auto* header = view.sub(0, 8).data;
            const unsigned length = AV_RB32(header);
            // Extended sizes inside this bounded moov are outside the subset.
            require(length >= 8 && length <= view.size, "nested_box_size");
            boxes_.push_back({{reinterpret_cast<const char*>(header + 4), 4}, view.sub(8, length - 8)});
            view = view.tail(length);
        }
    }
    View one(std::string_view tag, bool required = true) const {
        View found;
        for (const auto& b : boxes_) {
            if (b.tag == tag) {
                require(!found, "duplicate_box");
                found = b.contents;
            }
        }
        require(!required || static_cast<bool>(found), "missing_box");
        return found;
    }
    const std::vector<Box>& all() const { return boxes_; }

private:
    std::vector<Box> boxes_;
};

struct Movie {
    std::vector<std::uint8_t> metadata;
    U64 dataBegin = 0;
    U64 dataEnd = 0;
};

Movie readMovie(const AMBI_SourceV1& input, U64& consumed, const std::function<bool()>& canceled,
                bool metadataOnly = false) {
    auto read = [&](U64 pos, std::uint8_t* buf, std::size_t n) {
        require(n <= readBudget - consumed, "read_budget");
        while (n) {
            pollCancel(canceled);
            const auto request = static_cast<std::uint32_t>(std::min<std::size_t>(n, 65536));
            std::uint32_t actual = 0;
            const auto status = Input::readSource(input, pos, buf, request, actual);
            if (status == AMBI_CANCELED)
                throw Input::ReaderInputError(Input::ReaderInputFailure::Canceled,
                                              "MP4 audio index inspection canceled");
            if (status != AMBI_OK || actual != request)
                throw Input::ReaderInputError(Input::ReaderInputFailure::InputFailed,
                                              "cannot read MP4 audio index");
            consumed += actual;
            pos += actual;
            buf += actual;
            n -= actual;
        }
    };
    Movie movie;
    std::array<std::uint8_t, 8> buffer{};
    unsigned boxes = 0;
    for (U64 pos = 0; pos < input.byteSize;) {
        require(++boxes <= 256 && input.byteSize - pos >= 8, "top_level_layout");
        read(pos, buffer.data(), 8);
        const std::string_view tag(reinterpret_cast<const char*>(buffer.data() + 4), 4);
        // Keep tag tests before reusing the header buffer for extended sizes.
        const bool moov = tag == "moov", mdat = tag == "mdat";
        require(metadataOnly || moov || mdat || tag == "ftyp" || tag == "free" || tag == "skip" || tag == "wide",
                "unsupported_top_level");
        U64 length = AV_RB32(buffer.data()), header = 8;
        if (length == 1) {
            require(input.byteSize - pos >= 16, "top_level_layout");
            read(pos + 8, buffer.data(), 8);
            length = AV_RB64(buffer.data());
            header = 16;
        } else if (!length) {
            length = input.byteSize - pos;
        }
        require(length >= header && length <= input.byteSize - pos, "top_level_layout");
        if (moov) {
            require(movie.metadata.empty() && length > header, "moov_count");
            require(length - header <= readBudget - consumed, "read_budget");
            movie.metadata.resize(static_cast<std::size_t>(length - header));
            read(pos + header, movie.metadata.data(), movie.metadata.size());
            if (metadataOnly)
                return movie;
        }
        if (mdat && !metadataOnly) {
            require(!movie.dataEnd && length > header, "multiple_or_empty_mdat");
            movie.dataBegin = pos + header;
            movie.dataEnd = pos + length;
        }
        pos += length;
    }
    require(!movie.metadata.empty() && movie.dataEnd, "missing_moov_or_mdat");
    return movie;
}

struct TrackTime {
    unsigned scale;
    U64 duration;
};
TrackTime timeHeader(View v) {
    require(v.u32(0) == 0 || v.u32(0) == 0x01000000, "time_version");
    const bool wide = v.byte(0) == 1;
    const auto scale = v.u32(wide ? 20 : 12);
    require(scale > 0, "time_scale");
    return {scale, wide ? v.u64(24) : v.u32(16)};
}

View descriptor(View bytes, unsigned kind) {
    require(bytes.byte(0) == kind, "audio_descriptor");
    unsigned length = 0;
    for (unsigned i = 1; i <= 4; ++i) {
        const unsigned b = bytes.byte(i);
        length = (length << 7) | (b & 127);
        if (!(b & 128))
            return bytes.sub(i + 1, length);
    }
    throw Unsupported{"audio_descriptor_length"};
}

void validateAacConfig(View esds, const AVCodecParameters& codec) {
    require(esds.u32(0) == 0, "esds_version");
    const auto es = descriptor(esds.tail(4), 3);
    require(!(es.byte(2) & 0xe0), "es_descriptor_flags");
    const auto dec = descriptor(es.tail(3), 4);
    require(dec.u16(0) == 0x4015, "esds_stream_type");
    const auto asc = descriptor(dec.tail(13), 5);
    require(codec.extradata && codec.extradata_size > 0 &&
                asc.equals(codec.extradata, static_cast<unsigned>(codec.extradata_size)),
            "decoder_config_mismatch");
    const unsigned config = asc.u16(0);
    constexpr int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                             22050, 16000, 12000, 11025, 8000,  7350};
    const unsigned frequency = (config >> 7) & 15;
    require((config >> 11) == 2 && frequency < std::size(rates) &&
                rates[frequency] == codec.sample_rate &&
                static_cast<int>((config >> 3) & 15) == codec.ch_layout.nb_channels && !(config & 7),
            "aac_lc_profile");
    // Only the two-byte AudioSpecificConfig or the standard explicit
    // syncExtension with SBR disabled. HE-AAC and unknown extensions fall back.
    constexpr std::uint8_t noSbr[] = {0x56, 0xe5, 0x00};
    require(asc.size == 2 || asc.tail(2).equals(noSbr, sizeof noSbr), "aac_extensions");
}

unsigned alacBlockLimit(View config, const AVCodecParameters& codec) {
    // The decoder receives the full 36-byte ALAC atom (size/tag followed by
    // the four version bytes and the 24-byte ALACSpecificConfig).
    require(codec.extradata && codec.extradata_size == 36 && config.size == 28 &&
                AV_RB32(codec.extradata) == 36 && !std::memcmp(codec.extradata + 4, "alac", 4) &&
                config.equals(codec.extradata + 8, 28),
            "alac_decoder_config");
    const unsigned block = config.u32(4), bits = config.byte(9);
    require(config.u32(0) == 0 && config.byte(8) == 0 && block > 0 && block <= 65536 &&
                (bits == 16 || bits == 20 || bits == 24 || bits == 32) &&
                (!codec.bits_per_raw_sample || static_cast<unsigned>(codec.bits_per_raw_sample) == bits) &&
                config.byte(13) == static_cast<unsigned>(codec.ch_layout.nb_channels) &&
                config.u32(24) == static_cast<unsigned>(codec.sample_rate),
            "alac_config_fields");
    return block;
}

struct SampleDescription {
    unsigned reference;
    unsigned blockLimit;
};

SampleDescription validateDescription(View stsd, const AVCodecParameters& codec) {
    require(stsd.u32(0) == 0 && stsd.u32(4) == 1, "multiple_descriptions");
    const Children entries(stsd.tail(8));
    require(entries.all().size() == 1, "multiple_descriptions");
    const bool alac = codec.codec_id == AV_CODEC_ID_ALAC;
    const auto audio = entries.one(alac ? "alac" : "mp4a");
    const unsigned version = audio.u16(8), entryChannels = audio.u16(16);
    // ISO sample entries can contain the conventional stereo default for mono.
    // The codec-specific configuration must identify the actual channel count.
    require(version <= 1 && (entryChannels == 2 || entryChannels == static_cast<unsigned>(codec.ch_layout.nb_channels)) &&
                audio.u32(24) == (static_cast<unsigned>(codec.sample_rate) << 16),
            "sample_entry_format");
    const Children extensions(audio.tail(version == 0 ? 28 : 44));
    const auto wave = extensions.one("wave", false);
    const auto tag = alac ? "alac" : "esds";
    const auto outer = extensions.one(tag, false);
    require(static_cast<bool>(wave) != static_cast<bool>(outer), "codec_config_location");
    const auto config = wave ? Children(wave).one(tag) : outer;
    if (alac)
        return {audio.u16(6), alacBlockLimit(config, codec)};
    validateAacConfig(config, codec);
    return {audio.u16(6), 1024};
}

void validateLocalReference(View dinf, unsigned referenceIndex) {
    const auto dref = Children(dinf).one("dref");
    require(referenceIndex == 1 && dref.u32(0) == 0 && dref.u32(4) == 1, "data_reference");
    const Children refs(dref.tail(8));
    require(refs.all().size() == 1, "data_reference");
    const auto& ref = refs.all().front();
    require((ref.tag == "url " || ref.tag == "alis") && ref.contents.size == 4 &&
                ref.contents.u32(0) == 1,
            "external_data_reference");
}

struct Edit {
    U64 start;
    U64 frames;
};
Edit exactEdit(View edts, const TrackTime& media, const TrackTime& movie, View tkhd, bool alac) {
    if (!edts)
        return {0, media.duration};
    const auto elst = Children(edts).one("elst");
    require(elst.u32(0) == 0 || elst.u32(0) == 0x01000000, "edit_version");
    const bool wide = elst.byte(0) == 1;
    require(elst.size == (wide ? 28u : 20u) && elst.u32(4) == 1 && elst.u32(wide ? 24 : 16) == 0x10000,
            "complex_edit");
    const U64 duration = wide ? elst.u64(8) : elst.u32(8);
    const U64 start = wide ? elst.u64(16) : elst.u32(12);
    // Only AAC may discard its one whole priming block. Every accepted ALAC
    // edit covers the complete native media and starts at sample zero.
    require((start == 0 || (!alac && start == 1024)) && start < media.duration && duration > 0 &&
                duration <= std::numeric_limits<U64>::max() / media.scale,
            "edit_bounds");
    const auto scaled = duration * media.scale;
    if (scaled % movie.scale == 0 && scaled / movie.scale == media.duration - start)
        return {start, media.duration - start};

    // MOV can express an untrimmed track's end rounded up to a whole movie tick.
    // Keep native audio duration only for a matching track-header end, <1 ms
    // of overhang and the unique first movie tick covering all native samples.
    require(alac && movie.scale >= 1000 && movie.scale < media.scale &&
                media.duration <= std::numeric_limits<U64>::max() / movie.scale,
            "ambiguous_edit_duration");
    const auto native = media.duration * movie.scale;
    const U64 trackEnd = tkhd.byte(0) == 1 ? tkhd.u64(28) : tkhd.u32(20);
    require(scaled > native && scaled - native < media.scale && trackEnd == duration &&
                movie.duration >= duration,
            "ambiguous_edit_duration");
    return {0, media.duration};
}

struct Table {
    View rows;
    unsigned count;
    unsigned stride;
};
Table fixedRows(View v, unsigned stride) {
    const auto count = v.u32(4);
    require(v.u32(0) == 0 && count > 0 && count <= maxEntries &&
                v.size == 8 + static_cast<std::size_t>(count) * stride,
            "table_shape");
    return {v.tail(8), count, stride};
}
void validateGroups(const Children& table, unsigned count) {
    const auto sgpd = table.one("sgpd", false), sbgp = table.one("sbgp", false);
    if (!sgpd && !sbgp)
        return;
    // One roll-recovery group (-1) shared by all packets.
    constexpr std::uint8_t roll[] = {1, 0, 0, 0, 'r', 'o', 'l', 'l', 0, 0, 0, 2, 0, 0, 0, 1, 0xff, 0xff};
    require(sgpd && sbgp && sgpd.equals(roll, sizeof roll) && sbgp.size == 20 && sbgp.u32(0) == 0 &&
                sbgp.sub(4, 4).equals("roll", 4) && sbgp.u32(8) == 1 && sbgp.u32(12) == count &&
                sbgp.u32(16) == 1,
            "sample_groups");
}

bool indexReferencesMissingBytes(AVStream* stream, U64 fileBytes,
                                 const std::function<bool()>& canceled) {
    const int entries = avformat_index_get_entries_count(stream);
    for (int i = entries - 1; i >= 0; --i) {
        if ((entries - 1 - i) % 1024 == 0)
            pollCancel(canceled);
        const auto* packet = avformat_index_get_entry(stream, i);
        if (!packet || packet->pos < 0 || packet->size <= 0)
            continue;
        if (packet->flags & AVINDEX_DISCARD_FRAME)
            continue;
        const auto position = static_cast<U64>(packet->pos);
        if (position > fileBytes || static_cast<unsigned>(packet->size) > fileBytes - position)
            return true;
    }
    return false;
}

// A missing playable packet is conclusive only when its offsets refer to this
// source. This check is independent of presentation timing and fast-path admission.
bool isLocalAudioTrack(const AMBI_SourceV1& input, const AVStream& selected, U64& consumed,
                       const std::function<bool()>& canceled) {
    const auto movie = readMovie(input, consumed, canceled, true);
    const Children atoms({movie.metadata.data(), movie.metadata.size()});
    require(!atoms.one("mvex", false) && !atoms.one("cmov", false), "fragmented_or_compressed_movie");
    bool found = false;
    for (const Box& box : atoms.all()) {
        if (box.tag != "trak")
            continue;
        const Children track(box.contents);
        const auto tkhd = track.one("tkhd");
        require(tkhd.byte(0) <= 1, "track_version");
        if (tkhd.u32(tkhd.byte(0) == 1 ? 20 : 12) != static_cast<unsigned>(selected.id))
            continue;
        require(!found, "duplicate_selected_id");
        const Children media(track.one("mdia"));
        require(media.one("hdlr").sub(8, 4).equals("soun", 4), "selected_track");
        const Children minf(media.one("minf"));
        const auto stsd = Children(minf.one("stbl")).one("stsd");
        require(stsd.u32(0) == 0 && stsd.u32(4) == 1, "multiple_descriptions");
        const Children entries(stsd.tail(8));
        require(entries.all().size() == 1, "multiple_descriptions");
        const auto kind = selected.codecpar->codec_id == AV_CODEC_ID_ALAC ? "alac" : "mp4a";
        validateLocalReference(minf.one("dinf"), entries.one(kind).u16(6));
        found = true;
    }
    return found;
}

void verifyInventory(const Children& table, AVStream* stream, const Movie& movie, const TrackTime& media,
                     const Edit& edit, unsigned blockLimit, const std::function<bool()>& canceled) {
    const auto stsz = table.one("stsz");
    const unsigned size = stsz.u32(4), count = stsz.u32(8);
    require(stsz.u32(0) == 0 && count && count <= maxEntries &&
                stsz.size == (size ? 12 : 12 + static_cast<std::size_t>(count) * 4) &&
                stream->nb_frames == count &&
                avformat_index_get_entries_count(stream) == static_cast<int>(count),
            "sample_count");
    const bool alac = stream->codecpar->codec_id == AV_CODEC_ID_ALAC;
    require(media.duration <= static_cast<U64>(count) * blockLimit, "physical_extent");
    if (alac) {
        require(!table.one("sbgp", false) && !table.one("sgpd", false), "alac_sample_groups");
    } else {
        // AAC LC has fixed blocks and can end with one partially presented block.
        require(media.duration > static_cast<U64>(count - 1) * blockLimit, "physical_extent");
        validateGroups(table, count);
    }
    const auto times = fixedRows(table.one("stts"), 8);
    const auto chunks = fixedRows(table.one("stsc"), 12);
    const auto small = table.one("stco", false), large = table.one("co64", false);
    require(static_cast<bool>(small) != static_cast<bool>(large), "chunk_table");
    const auto offsets = fixedRows(small ? small : large, small ? 4 : 8);
    require(chunks.rows.u32(0) == 1, "chunk_first");

    unsigned sample = 0, timeRow = 0, runLeft = 0, delta = 0;
    U64 timestamp = 0, previousEnd = movie.dataBegin;
    for (unsigned group = 0; group < chunks.count; ++group) {
        const auto offset = static_cast<std::size_t>(group) * 12;
        const unsigned first = chunks.rows.u32(offset), perChunk = chunks.rows.u32(offset + 4);
        const unsigned next =
            group + 1 < chunks.count ? chunks.rows.u32(offset + 12) : offsets.count + 1;
        require(first >= 1 && first < next && next <= offsets.count + 1 && perChunk &&
                    chunks.rows.u32(offset + 8) == 1,
                "chunk_mapping");
        for (unsigned chunk = first - 1; chunk < next - 1; ++chunk) {
            U64 position = small ? offsets.rows.u32(static_cast<std::size_t>(chunk) * 4)
                                 : offsets.rows.u64(static_cast<std::size_t>(chunk) * 8);
            require(position >= previousEnd && position < movie.dataEnd && perChunk <= count - sample,
                    "chunk_extent");
            for (unsigned p = 0; p < perChunk; ++p, ++sample) {
                if ((sample & 1023) == 0)
                    pollCancel(canceled);
                if (!runLeft) {
                    require(timeRow < times.count, "stts_entries");
                    const auto idx = static_cast<std::size_t>(timeRow++) * 8;
                    runLeft = times.rows.u32(idx);
                    delta = times.rows.u32(idx + 4);
                    require(runLeft > 0 && delta > 0, "stts_entries");
                    if (alac)
                        require(delta <= blockLimit, "alac_packet_duration");
                }
                --runLeft;
                const unsigned packetSize =
                    size ? size : stsz.u32(12 + static_cast<std::size_t>(sample) * 4);
                require(packetSize && packetSize <= movie.dataEnd - position, "sample_extent");
                // A valid recorder can quantize adjacent packet times (e.g.
                // 1008, 1056 at 48 kHz). Do not silently account for a whole
                // missing/extra access unit as timestamp jitter.
                if (!alac) {
                    const U64 physicalStart = static_cast<U64>(sample) * blockLimit;
                    require(timestamp < physicalStart + blockLimit && physicalStart < timestamp + blockLimit,
                            "packet_clock_drift");
                }
                const auto* packet = avformat_index_get_entry(stream, static_cast<int>(sample));
                const auto dts =
                    static_cast<std::int64_t>(timestamp) - static_cast<std::int64_t>(edit.start);
                require(
                    packet && packet->size > 0 && static_cast<unsigned>(packet->size) == packetSize &&
                        packet->pos == static_cast<std::int64_t>(position) && packet->timestamp == dts,
                    "demux_index_mismatch");
                // FFmpeg marks the one whole preroll block as discarded. A
                // playable packet flagged for discard requires the generic path.
                require(dts < 0 || !(packet->flags & AVINDEX_DISCARD_FRAME),
                        "discarded_presentation_packet");
                if (sample == 0 && edit.start)
                    require(delta == 1024 && (packet->flags & AVINDEX_DISCARD_FRAME),
                            "unproved_priming");
                position += packetSize;
                require(delta <= media.duration - timestamp, "stts_duration");
                timestamp += delta;
            }
            previousEnd = position;
        }
    }
    require(sample == count && timeRow == times.count && !runLeft && timestamp == media.duration,
            "table_totals");
}

bool localAudioCandidate(const AVFormatContext* format, const AVStream* audio) {
    if (!format || !format->iformat || !format->iformat->name ||
        std::string_view(format->iformat->name) != "mov,mp4,m4a,3gp,3g2,mj2" || !audio ||
        !audio->codecpar || audio->id <= 0 || audio->index < 0 ||
        static_cast<unsigned>(audio->index) >= format->nb_streams ||
        format->streams[audio->index] != audio)
        return false;
    const auto& codec = *audio->codecpar;
    return codec.codec_type == AVMEDIA_TYPE_AUDIO &&
           (codec.codec_id == AV_CODEC_ID_AAC || codec.codec_id == AV_CODEC_ID_ALAC);
}

bool eligibleForExactTiming(const AVFormatContext* format, const AVStream* audio) {
    const auto& codec = *audio->codecpar;
    if ((codec.codec_id == AV_CODEC_ID_AAC && codec.profile != AV_PROFILE_AAC_LOW) ||
        codec.sample_rate <= 0 || codec.sample_rate > 65535 ||
        (codec.ch_layout.nb_channels != 1 && codec.ch_layout.nb_channels != 2) ||
        codec.initial_padding || codec.trailing_padding || audio->start_time != 0 ||
        audio->duration <= 0 || audio->time_base.num != 1 || audio->time_base.den != codec.sample_rate)
        return false;
    unsigned audioTracks = 0;
    for (unsigned i = 0; i < format->nb_streams; ++i)
        if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            ++audioTracks;
    return audioTracks == 1;
}

U64 inspect(const AMBI_SourceV1& input, const AVFormatContext& format, const AVStream& selected,
            U64& consumed, U64& physicalFrames, const std::function<bool()>& canceled) {
    const auto movie = readMovie(input, consumed, canceled);
    const View metadata{movie.metadata.data(), movie.metadata.size()};
    // MOV may consume the iTunes priming override without exposing its length
    // as AVCodecParameters::initial_padding. Defer any such file to FFmpeg.
    constexpr std::string_view gaplessTag = "iTunSMPB";
    require(std::search(metadata.data, metadata.data + metadata.size, gaplessTag.begin(),
                        gaplessTag.end()) == metadata.data + metadata.size,
            "vendor_priming");
    const Children atoms(metadata);
    require(!atoms.one("mvex", false) && !atoms.one("cmov", false), "fragmented_or_compressed_movie");
    const auto movieTime = timeHeader(atoms.one("mvhd"));
    std::vector<unsigned> trackIds;
    U64 frames = 0;
    for (const Box& box : atoms.all()) {
        if (box.tag != "trak")
            continue;
        const Children track(box.contents);
        const auto tkhd = track.one("tkhd");
        require(tkhd.byte(0) <= 1, "track_version");
        const auto id = tkhd.u32(tkhd.byte(0) == 1 ? 20 : 12);
        require(id && std::find(trackIds.begin(), trackIds.end(), id) == trackIds.end(), "track_id");
        trackIds.push_back(id);
        const Children media(track.one("mdia"));
        const bool isAudio = media.one("hdlr").sub(8, 4).equals("soun", 4);
        require(isAudio == (id == static_cast<unsigned>(selected.id)), "selected_track");
        if (!isAudio)
            continue;
        const TrackTime timing = timeHeader(media.one("mdhd"));
        require(timing.scale == static_cast<unsigned>(selected.codecpar->sample_rate),
                "audio_timescale");
        const Children minf(media.one("minf"));
        const Children table(minf.one("stbl"));
        const auto description = validateDescription(table.one("stsd"), *selected.codecpar);
        validateLocalReference(minf.one("dinf"), description.reference);
        const bool alac = selected.codecpar->codec_id == AV_CODEC_ID_ALAC;
        const auto edit = exactEdit(track.one("edts", false), timing, movieTime, tkhd, alac);
        require(edit.frames == static_cast<U64>(selected.duration), "selected_duration");
        // Unknown sample groups, edit offsets, encryption and alternate sample
        // descriptions can affect delivery. Only mapped tables qualify.
        for (const Box& child : table.all()) {
            const auto tag = child.tag;
            require(tag == "stsd" || tag == "stts" || tag == "stsz" || tag == "stsc" || tag == "stco" ||
                        tag == "co64" || tag == "sgpd" || tag == "sbgp",
                    "unsupported_sample_table");
        }
        verifyInventory(table, format.streams[selected.index], movie, timing, edit, description.blockLimit, canceled);
        physicalFrames = alac ? timing.duration : static_cast<U64>(selected.nb_frames) * 1024 - edit.start;
        frames = edit.frames;
    }
    require(frames > 0, "selected_track_missing");
    return frames;
}
} // namespace

Mp4AudioIndexResult probeMp4AudioSampleTable(const Input::MediaInputSource& source,
                                         const AVFormatContext* format, const AVStream* audio,
                                         const std::function<bool()>& canceled) {
    Mp4AudioIndexResult result;
    if (!localAudioCandidate(format, audio))
        return result;
    auto inspectSource = [&](const AMBI_SourceV1& pinned) {
        if (indexReferencesMissingBytes(format->streams[audio->index], pinned.byteSize, canceled)) {
            bool local = false;
            try {
                local = isLocalAudioTrack(pinned, *audio, result.bytesRead, canceled);
            } catch (const Unsupported&) {
                // The index already points beyond EOF. A packet scan over the
                // surviving prefix cannot resolve the source of those offsets.
            }
            throw Input::ReaderInputError(
                Input::ReaderInputFailure::InputFailed,
                local ? "Incomplete audio import: the selected audio track references data beyond the end of the file."
                      : "Possibly incomplete audio import: the selected audio index references unavailable bytes "
                        "and its data source could not be confirmed.");
        }
        try {
            if (!eligibleForExactTiming(format, audio))
                return;
            U64 physical = 0;
            result.frames = inspect(pinned, *format, *audio, result.bytesRead, physical, canceled);
            result.physicalFrames = physical;
            const bool alac = audio->codecpar->codec_id == AV_CODEC_ID_ALAC;
            result.source = alac ? PresentationTotalSource::Mp4AlacSampleTablePresentation
                                 : PresentationTotalSource::Mp4AacSampleEditTablePresentation;
            result.reason = alac ? "validated_alac_sample_tables" : "validated_aac_sample_edit_tables";
        } catch (const Unsupported& skipped) {
            result.reason = skipped.reason;
        }
    };
    if (const auto* pinned = source.stableSource()) {
        inspectSource(*pinned);
    } else {
        Input::DemuxByteSource borrowed(format->pb);
        if (!borrowed.valid())
            return result;
        const auto sameHandle = borrowed.source(source.displayLabel());
        inspectSource(*sameHandle.stableSource());
        if (!borrowed.restore())
            throw Input::ReaderInputError(
                Input::ReaderInputFailure::InputFailed,
                "cannot restore audio input position after MP4 index inspection");
    }
    return result;
}

} // namespace AveMediaBridge::Probe
