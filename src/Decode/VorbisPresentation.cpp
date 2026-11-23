#include "Decode/VorbisInput.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <numeric>
#include <set>
#include <vorbis/vorbisfile.h>
extern "C" {
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
namespace AveMediaBridge::Vorbis {
namespace {
using detail::Bytes;
using detail::require;
// Xiph revisits the same page while locating logical stream boundaries.
// Bound both the distinct input blocks and the discontinuous reads.
enum class ProbeLimit { none, bytes, seeks, links };
struct File {
    static constexpr int64_t blockSize = 4096;
    static constexpr size_t blocks = 256;
    detail::Source &source;
    int64_t length, position = 0, physical = -1;
    bool failed = false;
    ProbeLimit limit = ProbeLimit::none;
    unsigned seeks = 0;
    std::array<int64_t, blocks> keys{};
    size_t used = 0;
    Bytes cache;

    explicit File(detail::Source &file) : source(file), length(static_cast<int64_t>(file.size())) {
        require(length > 0, "empty input");
        cache.resize(static_cast<size_t>(std::min<int64_t>((length - 1) / blockSize + 1, blocks) * blockSize));
    }
    bool fail() {
        failed = true;
        errno = EIO;
        return false;
    }
    bool hitLimit(ProbeLimit reached) {
        limit = reached;
        return fail();
    }
    bool copy(int64_t at, void *dest, size_t count) {
        if (failed || at < 0 || at > length || count > static_cast<uint64_t>(length - at))
            return fail();
        auto *out = static_cast<uint8_t *>(dest);
        while (count) {
            const int64_t block = at - at % blockSize;
            const auto end = keys.begin() + used;
            const size_t slot = std::find(keys.begin(), end, block) - keys.begin();
            if (slot == used) {
                if (used == blocks)
                    return hitLimit(ProbeLimit::bytes);
                if (physical != block) {
                    if (seeks >= 128)
                        return hitLimit(ProbeLimit::seeks);
                    ++seeks;
                }
                const auto bytes = static_cast<size_t>(std::min(blockSize, length - block));
                keys[used++] = block;
                if (!source.readAt(block, cache.data() + slot * blockSize, static_cast<uint32_t>(bytes)))
                    return fail();
                physical = block + bytes;
            }
            const auto offset = static_cast<size_t>(at - block);
            const auto part = std::min(count, static_cast<size_t>(blockSize) - offset);
            memcpy(out, cache.data() + slot * blockSize + offset, part);
            out += part;
            at += part;
            count -= part;
        }
        return true;
    }
};
static size_t readCb(void *dst, size_t size, size_t items, void *opaque) {
    auto &a = *static_cast<File *>(opaque);
    if (a.failed || size != 1) {
        a.fail();
        return 0;
    }
    const auto n = static_cast<size_t>(std::min<uint64_t>(items, a.length - a.position));
    if (!a.copy(a.position, dst, n))
        return 0;
    a.position += n;
    return n;
}
static int seekCb(void *opaque, ogg_int64_t offset, int whence) {
    auto &a = *static_cast<File *>(opaque);
    const int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? a.position : a.length;
    if (a.failed || (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) || offset < -base ||
        offset > a.length - base) {
        a.fail();
        return -1;
    }
    a.position = base + offset;
    return 0;
}
static ogg_int64_t tellCb(void *opaque) {
    const auto &a = *static_cast<File *>(opaque);
    return a.failed ? -1 : a.position;
}

static void verifyEnd(File &input, int64_t end, long serial) {
    constexpr size_t maxPage = 27 + 255 + 255 * 255;
    Bytes tail(static_cast<size_t>(std::min<int64_t>(maxPage, end)));
    require(input.copy(end - tail.size(), tail.data(), tail.size()), "read terminal page");
    for (size_t i = tail.size(); i-- > 0;) {
        const size_t left = tail.size() - i;
        const uint8_t *p = tail.data() + i;
        if (left < 27 || memcmp(p, "OggS", 4) || p[4] || (p[5] & 4) == 0 ||
            detail::little32(p + 14) != static_cast<uint32_t>(serial))
            continue;
        size_t size = 27 + p[26];
        if (size > left)
            continue;
        for (int j = 0; j < p[26]; ++j)
            size += p[27 + j];
        if (size != left)
            continue;
        Bytes page(p, p + size);
        const auto crc = detail::little32(p + 22);
        memset(page.data() + 22, 0, 4);
        if (detail::pageChecksum(page) == crc)
            return;
    }
    throw std::runtime_error("missing complete terminal Vorbis page");
}

// Validate small unsupported presentations completely before categorizing their format.
// In particular, Xiph can collapse duplicate serial numbers into one reported link.
static void verifySmallPages(File &f, long expectedLinks) {
    int64_t off = 0;
    std::set<uint32_t> seen;
    uint32_t serial = 0, sequence = 0;
    bool active = false;
    while (off < f.length) {
        uint8_t header[27];
        require(f.copy(off, header, sizeof header) && memcmp(header, "OggS", 4) == 0 && header[4] == 0 &&
                    !(header[5] & ~7),
                "invalid or incomplete page header");
        const int segs = header[26];
        uint8_t lacing[255];
        require(f.copy(off + 27, lacing, segs), "invalid lacing");
        size_t size = 27 + segs;
        for (int i = 0; i < segs; ++i)
            size += lacing[i];
        Bytes page(size);
        require(f.copy(off, page.data(), size), "incomplete page");
        const auto crc = detail::little32(header + 22);
        memset(page.data() + 22, 0, 4);
        require(detail::pageChecksum(page) == crc, "page CRC");
        if (header[5] & 2) {
            serial = detail::little32(header + 14);
            require(!active && seen.insert(serial).second && !(header[5] & 1), "invalid logical stream");
            active = true;
            sequence = 0;
        }
        require(active && detail::little32(header + 14) == serial && detail::little32(header + 18) == sequence++,
                "invalid page sequence");
        if (header[5] & 4)
            active = false;
        off += size;
    }
    require(!active && seen.size() == static_cast<size_t>(expectedLinks), "unclosed logical stream");
}
static void finishDuration(Plan &p) {
    int64_t hz = 1;
    for (const auto &part : p.parts) {
        const int64_t mul = part.rate / std::gcd(hz, static_cast<int64_t>(part.rate));
        require(hz <= INT64_MAX / mul, "source clock overflow");
        hz *= mul;
    }
    int64_t ticks = 0;
    for (auto &part : p.parts) {
        const int64_t scale = hz / part.rate;
        require(part.frames <= (INT64_MAX - ticks) / scale, "duration overflow");
        ticks += part.frames * scale;
        const int64_t bound = av_rescale_rnd(ticks, p.rate, hz, AV_ROUND_NEAR_INF);
        require(bound > p.frames && bound <= INT64_MAX / (4 * p.channels), "output extent");
        part.output = bound - p.frames;
        p.frames = bound;
    }
}

static Plan readPlan(File &a) {
    ov_callbacks cb{readCb, seekCb, nullptr, tellCb, [](void* opaque) {
        auto& file = *static_cast<File*>(opaque);
        if (!file.failed) file.hitLimit(ProbeLimit::links);
    }};
    OggVorbis_File vf{};
    require(ov_open_callbacks(&a, &vf, nullptr, 0, cb) == 0, "64bit Xiph metadata");
    struct Release {
        OggVorbis_File *vf;
        ~Release() { ov_clear(vf); }
    } release{&vf};
    Plan p;
    std::set<long> serials;
    const long links = ov_streams(&vf);
    require(links > 0 && links <= 128, "unsupported link count");
    p.ordinarySingleLink = links == 1 && vf.first_audio_origin_kind == AMBR_VORBIS_ZERO_ORIGIN;
    p.singlePageMultichannel = links == 1 && vf.first_audio_origin_kind == AMBR_VORBIS_TERMINAL_AUDIO_PAGE;
    for (int i = 0; i < links; ++i) {
        auto *v = ov_info(&vf, i);
        const auto n = ov_pcm_total(&vf, i);
        require(v && v->rate > 0 && v->rate <= 384000 && v->channels > 0 && v->channels <= 255 && n > 0 &&
                    serials.insert(ov_serialnumber(&vf, i)).second,
                "unsupported link format or count");
        if (v->channels > 2 && !p.unsupportedChannels)
            p.unsupportedChannels = v->channels;
        p.rate = std::max(p.rate, int(v->rate));
        p.channels = std::max(p.channels, v->channels);
        p.parts.push_back({int(v->rate), v->channels, n, 0});
    }
    if (p.unsupportedChannels) {
        if (a.length <= int64_t(File::blocks) * File::blockSize)
            verifySmallPages(a, links);
        else
            for (int i = 0; i < links; ++i) {
                const auto boundary = i + 1 < links ? vf.offsets[i + 1] : a.length;
                require(boundary > 0 && boundary <= a.length, "invalid link boundary");
                verifyEnd(a, boundary, ov_serialnumber(&vf, i));
            }
    }
    finishDuration(p);
    require(!a.failed, "metadata read limits");
    return p;
}

struct Normalize {
    const Plan &p;
    void *user;
    Emit emit;
    int index = -1;
    uint64_t bytes = 0;
    int64_t written = 0, received = 0;
    SwrContext *swr = nullptr;
    std::vector<float> converted, stereo;
    // Save only the native frames that fit inside the filter's initial lookahead.
    std::vector<float> startup;
    int startupBudget = 0;
    ~Normalize() { swr_free(&swr); }
    void deliver(const float *data, int count) {
        const auto &part = p.parts.at(index);
        const int n = static_cast<int>(std::min<int64_t>(count, part.output - written));
        if (!n)
            return;
        if (part.channels == 1 && p.channels == 2) {
            stereo.resize(2 * size_t(n));
            for (int i = 0; i < n; ++i)
                stereo[2 * i] = stereo[2 * i + 1] = data[i];
            data = stereo.data();
        }
        require(emit(user, data, n, p.channels, p.rate, index, bytes) == 0, "consumer stopped");
        written += n;
    }
    int convert(const float *data, int frames) {
        const int space = swr_get_out_samples(swr, frames);
        require(space >= 0, "output sample bound");
        const int capacity =
            static_cast<int>(std::min<int64_t>(std::max(128, space), p.parts[index].output - written));
        if (!capacity)
            return 0;
        converted.resize(size_t(capacity) * p.parts[index].channels);
        uint8_t *dst = reinterpret_cast<uint8_t *>(converted.data());
        const uint8_t *src = reinterpret_cast<const uint8_t *>(data);
        int n = swr_convert(swr, &dst, capacity, data ? &src : nullptr, frames);
        require(n >= 0, "resampler output");
        deliver(converted.data(), n);
        return n;
    }
    void finish() {
        if (index < 0)
            return;
        const auto &part = p.parts[index];
        require(received == part.frames, "Xiph link length differs from native decoded length");
        if (swr) {
            if (!startup.empty()) {
                // Finish an isolated short link with its own reflected right boundary.
                // Example: a b c | c b a a b c. Extension supplies FIR context only.
                const int channels = part.channels;
                const int frames = static_cast<int>(startup.size() / channels);
                std::vector<float> edge(size_t(startupBudget) * channels);
                for (int i = 0; i < startupBudget; ++i) {
                    const int across = i % (2 * frames);
                    const int source = across < frames ? frames - 1 - across : across - frames;
                    std::copy_n(&startup[size_t(source) * channels], channels, &edge[size_t(i) * channels]);
                }
                convert(edge.data(), startupBudget);
            }
            while (convert(nullptr, 0) > 0) {
            }
            swr_free(&swr);
        }
        startup.clear();
        require(written == part.output, "resampler returned too few timeline frames");
    }
    void begin(int link, int rate, int channels) {
        require(link == index + 1 && size_t(link) < p.parts.size(), "link sequence");
        const auto &part = p.parts[link];
        require(rate == part.rate && channels == part.channels, "stream identity differs from metadata");
        finish();
        index = link;
        written = 0;
        received = 0;
        if (rate != p.rate) {
            AVChannelLayout layout{};
            av_channel_layout_default(&layout, channels);
            int rc = swr_alloc_set_opts2(&swr, &layout, AV_SAMPLE_FMT_FLT, p.rate, &layout, AV_SAMPLE_FMT_FLT,
                                         rate, 0, nullptr);
            av_channel_layout_uninit(&layout);
            require(rc >= 0 && swr_init(swr) == 0, "resampler initialization");
            int64_t taps = 0;
            require(av_opt_get_int(swr, "filter_size", 0, &taps) == 0 && taps > 0 && taps <= 1024, "filter size");
            // The document chooses the highest input rate; all conversions upsample.
            // Four filter widths cover native startup and the right-hand lookahead.
            startupBudget = 4 * static_cast<int>(taps) + 4;
        }
    }
    static int __cdecl input(void *ctx, const float *data, int frames, int channels, int rate, int link,
                             uint64_t bytes) {
        try {
            auto &self = *static_cast<Normalize *>(ctx);
            if (link != self.index)
                self.begin(link, rate, channels);
            self.bytes = bytes;
            require(frames > 0 && self.received <= self.p.parts[link].frames - frames, "excess source frames");
            self.received += frames;
            if (self.swr) {
                self.convert(data, frames);
                if (!self.written) {
                    require(self.received <= self.startupBudget, "resampler startup exceeded bounded lookahead");
                    self.startup.insert(self.startup.end(), data, data + size_t(frames) * channels);
                } else
                    self.startup.clear();
            } else
                self.deliver(data, frames);
            return 0;
        } catch (const std::exception &) {
            return 1;
        }
    }
};
} // namespace

Status probe(const Input::MediaInputSource &input, Plan &result) {
    result = {};
    try {
        detail::Source source(input);
        if (source.status() != Status::Ok)
            return source.status();
        File file(source);
        try {
            result = readPlan(file);
            return result.unsupportedChannels ? Status::UnsupportedChannels : Status::Ok;
        } catch (const std::exception &) {
            if (source.status() != Status::Ok)
                return source.status();
            if (file.limit != ProbeLimit::none)
                return Status::QuickScanLimit;
            return Status::Invalid;
        }
    } catch (const std::exception &) {
        return Status::IoError;
    }
}

Status measure(const Input::MediaInputSource &input, Plan &result) {
    result = {};
    try {
        detail::Source source(input);
        if (source.status() != Status::Ok) return source.status();
        Plan measured;
        uint64_t bytes = 0;
        const auto status = detail::readPackets(source, nullptr, nullptr, nullptr, nullptr, bytes, &measured);
        if (status != Status::Ok) return status;
        require(bytes == source.size() && !measured.parts.empty(), "incomplete Vorbis presentation");
        finishDuration(measured);
        result = std::move(measured);
        return result.unsupportedChannels ? Status::UnsupportedChannels : Status::Ok;
    } catch (const std::bad_alloc &) {
        return Status::IoError;
    } catch (const std::exception &) {
        return Status::Invalid;
    }
}

Status decode(const Input::MediaInputSource &input, const Plan &plan, void *user, Emit emit, Cancel cancel,
              uint64_t &bytes) {
    bytes = 0;
    if (!emit || plan.parts.empty() || plan.unsupportedChannels)
        return Status::Invalid;
    try {
        detail::Source source(input);
        if (source.status() != Status::Ok)
            return source.status();
        Normalize normalizer{plan, user, emit};
        const auto rc = detail::readPackets(source, &normalizer, Normalize::input, user, cancel, bytes);
        if (rc != Status::Ok)
            return rc;
        normalizer.finish();
        require(normalizer.index + 1 == int(plan.parts.size()), "missing links");
        return Status::Ok;
    } catch (const std::exception &) {
        return Status::Invalid;
    }
}
} // namespace AveMediaBridge::Vorbis
