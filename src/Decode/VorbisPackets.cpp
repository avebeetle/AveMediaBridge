#include "Decode/VorbisInput.hpp"
#include "Decode/VorbisCommentPacket.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/vorbis_parser.h>
#include <libavutil/mem.h>
}
namespace AveMediaBridge::Vorbis::detail {
uint32_t little32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint32_t pageChecksum(const Bytes &page) {
    static const auto lut = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t x = i << 24;
            for (int b = 0; b < 8; ++b)
                x = (x << 1) ^ ((x >> 31) ? 0x04c11db7u : 0u);
            t[i] = x;
        }
        return t;
    }();
    uint32_t crc = 0;
    for (auto b : page)
        crc = (crc << 8) ^ lut[(crc >> 24) ^ b];
    return crc;
}
// Sequential reads are batched, including for a pinned callback source. The Ogg
// parser can request the fixed header, lacing and payload separately for each page.
struct Reader {
    Source &source;
    uint64_t bytes = 0;
    std::array<uint8_t, 64 * 1024> cache;
    size_t used = 0, available = 0;

    bool read(void *dest, size_t size) {
        auto *data = static_cast<uint8_t *>(dest);
        while (size) {
            if (used == available) {
                available = static_cast<size_t>(std::min<uint64_t>(source.size() - bytes, cache.size()));
                used = 0;
                if (!available || !source.readAt(bytes, cache.data(), static_cast<uint32_t>(available)))
                    return false;
            }
            const size_t chunk = std::min(size, available - used);
            std::memcpy(data, cache.data() + used, chunk);
            bytes += chunk;
            used += chunk;
            size -= chunk;
            data += chunk;
        }
        return true;
    }
    bool page(Bytes &page) {
        if (bytes == source.size())
            return false;
        page.resize(27);
        require(read(page.data(), 27) && !memcmp(page.data(), "OggS", 4) && page[4] == 0, "page header");
        const size_t count = page[26];
        page.resize(27 + count);
        require(read(page.data() + 27, count), "page lacing");
        size_t payload = 0;
        for (size_t i = 0; i < count; ++i)
            payload += page[27 + i];
        const size_t header = page.size();
        page.resize(header + payload);
        require(read(page.data() + header, payload), "page payload");
        const auto expected = little32(page.data() + 22);
        memset(page.data() + 22, 0, 4);
        require(pageChecksum(page) == expected, "page CRC");
        return true;
    }
};
struct NativeLink {
    std::array<Bytes, 3> headers;
    int h = 0, channels = 0, rate = 0;
    uint64_t audioPackets = 0;
    int64_t pos = 0;
    bool anchored = false, continued = false;
    Plan* measurement;
    int64_t pageFrames = 0;
    AVVorbisParseContext* parser = nullptr;
    CommentPacket comment;
    explicit NativeLink(Plan* plan) : measurement(plan) {}
    AVCodecContext *dec = nullptr;
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    Bytes fragment;
    std::vector<float> pcm;
    ~NativeLink() {
        av_vorbis_parse_free(&parser);
        avcodec_free_context(&dec);
        av_frame_free(&frame);
        av_packet_free(&pkt);
    }

    void openDecoder() {
        require(headers[0].size() == 30, "identification length");
        channels = headers[0][11];
        rate = int(little32(headers[0].data() + 12));
        require(channels > 0 && (measurement || channels <= 2) && rate > 0 && rate <= 384000,
                "unsupported channel layout/rate");
        Bytes extra{2};
        for (int i = 0; i < 2; ++i) {
            size_t n = headers[i].size();
            while (n >= 255) {
                extra.push_back(255);
                n -= 255;
            }
            extra.push_back(uint8_t(n));
        }
        for (auto &item : headers)
            extra.insert(extra.end(), item.begin(), item.end());
        if (measurement) {
            const int size = static_cast<int>(extra.size());
            extra.resize(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE);
            parser = av_vorbis_parse_init(extra.data(), size);
            require(parser, "invalid Vorbis setup");
            measurement->parts.push_back({rate, channels, 0, 0});
            measurement->rate = std::max(measurement->rate, rate);
            measurement->channels = std::max(measurement->channels, channels);
            if (channels > 2 && !measurement->unsupportedChannels) measurement->unsupportedChannels = channels;
            return;
        }
        const auto *codec = avcodec_find_decoder(AV_CODEC_ID_VORBIS);
        require(codec && (dec = avcodec_alloc_context3(codec)) && pkt && frame, "decoder allocation");
        dec->extradata = static_cast<uint8_t *>(av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        require(dec->extradata, "extradata allocation");
        memcpy(dec->extradata, extra.data(), extra.size());
        dec->extradata_size = int(extra.size());
        require(avcodec_open2(dec, codec, nullptr) >= 0, "decoder open");
    }
    void receive() {
        int rc;
        while ((rc = avcodec_receive_frame(dec, frame)) == 0) {
            require(frame->format == AV_SAMPLE_FMT_FLTP && frame->ch_layout.nb_channels == channels &&
                        frame->sample_rate == rate && frame->nb_samples > 0,
                    "PCM format");
            const auto first = pcm.size();
            pcm.resize(first + size_t(frame->nb_samples) * channels);
            for (int c = 0; c < channels; ++c) {
                const auto *plane = reinterpret_cast<const float *>(frame->extended_data[c]);
                for (int i = 0; i < frame->nb_samples; ++i)
                    pcm[first + size_t(i) * channels + c] = plane[i];
            }
            av_frame_unref(frame);
        }
        require(rc == AVERROR(EAGAIN) || rc == AVERROR_EOF, "decode frame");
    }
    void append(const uint8_t* data, uint8_t count) {
        continued = count == 255;
        if (h == 1) {
            comment.append(data, count);
        } else {
            require(fragment.size() + count <= 4u * 1024u * 1024u, "packet exceeds supported size");
            fragment.insert(fragment.end(), data, data + count);
        }
    }
    void completePacket() {
        if (h == 1) {
            require(comment.complete(), "incomplete Vorbis comment");
            fragment = {3,'v','o','r','b','i','s',0,0,0,0,0,0,0,0,1};
        }
        if (h < 3) {
            require(fragment.size() >= 7 && fragment[0] == uint8_t(1 + 2 * h) &&
                        !memcmp(fragment.data() + 1, "vorbis", 6),
                    "Vorbis header order");
            headers[h++].swap(fragment);
            if (h == 3)
                openDecoder();
            return;
        }
        require(!fragment.empty() && !(fragment[0] & 1), "audio packet type");
        if (measurement) {
            const int duration = av_vorbis_parse_frame(parser, fragment.data(), static_cast<int>(fragment.size()));
            require(duration > 0, "invalid Vorbis audio mode");
            if (audioPackets++) pageFrames += duration; // first packet primes the overlap
            fragment.clear();
            return;
        }
        require(av_new_packet(pkt, int(fragment.size())) == 0, "packet allocation");
        memcpy(pkt->data, fragment.data(), fragment.size());
        require(avcodec_send_packet(dec, pkt) >= 0, "decode packet");
        av_packet_unref(pkt);
        receive();
        ++audioPackets;
        fragment.clear();
    }
    void emitPage(int64_t gp, bool eos, int index, Reader &r, Emit emit, void *user) {
        if (eos) {
            require((parser || dec) && !continued && audioPackets > 0, "incomplete last page");
            if (dec) {
                require(avcodec_send_packet(dec, nullptr) >= 0, "send EOF");
                receive();
            }
        }
        const auto count = measurement ? pageFrames : static_cast<int64_t>(pcm.size() / std::max(1, channels));
        if (!count) {
            require(!eos || (gp == pos && anchored), "missing final samples");
            return;
        }
        require(gp >= 0, "missing granule for completed audio");
        if (!anchored) {
            pos = eos ? 0 : gp - count;
            if (pos != 0)
                require(audioPackets == 2 && (pos >= 0 || -pos <= count), "unsupported beginning");
            if (measurement && index == 0) {
                measurement->ordinarySingleLink = pos == 0 && !eos;
                measurement->singlePageMultichannel = eos;
            }
            anchored = true;
        }
        require(pos <= std::numeric_limits<int64_t>::max() - count, "position overflow");
        const int64_t end = pos + count;
        require(eos ? (gp >= std::max<int64_t>(0, pos) && gp <= end) : gp == end, "granule continuity");
        const int64_t start = std::max<int64_t>(0, pos);
        const int64_t frames = gp - start;
        if (frames > 0) {
            if (measurement) {
                auto &total = measurement->parts.back().frames;
                require(total <= INT64_MAX - frames, "Vorbis length overflow");
                total += frames;
            } else {
                require(emit(user, pcm.data() + size_t(start - pos) * channels, int(frames), channels, rate,
                             index, r.bytes) == 0, "consumer stopped");
            }
        }
        pos = gp;
        pageFrames = 0;
        pcm.clear();
    }
};

Status readPackets(Source &source, void *user, Emit emit, void *cancelUser, Cancel cancel, uint64_t &sourceBytes, Plan* measured) {
    Reader in{source};
    try {
        std::unique_ptr<NativeLink> link;
        Bytes page;
        std::set<uint32_t> serials;
        uint32_t serial = 0, next = 0;
        int index = -1;
        while (in.bytes < source.size()) {
            if (cancel && cancel(cancelUser))
                return Status::Cancelled;
            require(in.page(page), "read Ogg page");
            const uint8_t flags = page[5];
            const bool bos = (flags & 2) != 0, eos = (flags & 4) != 0;
            require(!(flags & ~7), "page flags");
            if (bos) {
                serial = little32(page.data() + 14);
                require(!link && serials.insert(serial).second && !(flags & 1), "unexpected BOS");
                // Bound the per-stream serial set and prepared plan independently of file size.
                if (measured && serials.size() > 65536) return Status::ExtendedLimit;
                if (measured && index >= 0) {
                    measured->ordinarySingleLink = false;
                    measured->singlePageMultichannel = false;
                }
                link = std::make_unique<NativeLink>(measured);
                next = 0;
                ++index;
            }
            require(link && little32(page.data() + 14) == serial && little32(page.data() + 18) == next++,
                    "link order");
            require(bool(flags & 1) == link->continued, "continued packet");
            int64_t gp;
            memcpy(&gp, page.data() + 6, 8);
            size_t at = 27 + page[26];
            for (unsigned i = 0; i < page[26]; ++i) {
                const auto n = page[27 + i];
                link->append(page.data() + at, n);
                at += n;
                if (n < 255)
                    link->completePacket();
            }
            link->emitPage(gp, eos, index, in, emit, user);
            if (eos)
                link.reset();
        }
        require(!link && index >= 0, "missing end of stream");
        sourceBytes = in.bytes;
        return Status::Ok;
    } catch (const std::exception &) {
        sourceBytes = in.bytes;
        return source.status() == Status::Ok ? Status::Invalid : source.status();
    }
}
} // namespace AveMediaBridge::Vorbis::detail
