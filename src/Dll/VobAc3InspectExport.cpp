#include "AveMediaBridge/AveMediaBridgeSourceInspectApi.h"
#include "Probe/QueuedSourceSha256.hpp"

extern "C" {
#include <libavcodec/ac3_parser.h>
#include <libavcodec/defs.h>
#include <libavutil/crc.h>
}
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
using Byte = std::uint8_t;
using Count = std::uint64_t;
constexpr unsigned sectorBytes = 2048;

unsigned big16(const Byte* p) { return (unsigned{p[0]} << 8) | p[1]; }

// Accept the DVD audio PES headers whose optional fields we can fully parse.
bool supportedAudioPesHeader(const Byte* p, unsigned bytes) {
    if (bytes < 7 || p[0] != 0x80 || 3u + p[2] + 4u > bytes) return false;
    const unsigned flags = p[1], end = 3 + p[2];
    if (flags != 0 && flags != 0x80 && flags != 0x81) return false;
    unsigned pos = 3;
    if (flags & 0x80) {
        if (end - pos < 5 || (p[pos] & 0xf1) != 0x21 || !(p[pos + 2] & 1) || !(p[pos + 4] & 1))
            return false;
        pos += 5;
    }
    if (flags & 1) {
        if (end - pos < 3 || p[pos] != 0x10 || (p[pos + 1] & 0xc0) != 0x40) return false;
        pos += 3;
    }
    return std::all_of(p + pos, p + end, [](Byte b) { return b == 0xff; });
}

// A restricted DVD pack has a single selected AC-3 substream. Verify its PES
// frame-start counts and offsets against the elementary syncframes themselves.
// An unknown sector, incomplete frame or channel/rate change drops the proof.
struct DvdAudio {
    Count frames = 0;
    int rate = 0, channels = 0, streamIndex = -1, audioId = -1, videoId = -1;
    unsigned used = 0, needed = 7;
    std::array<Byte, 3840 + AV_INPUT_BUFFER_PADDING_SIZE> frame{};
    const AVCRC* crc = av_crc_get_table(AV_CRC_16_ANSI);

    bool audioPes(const Byte* p, unsigned n) {
        if (!supportedAudioPesHeader(p, n)) return false;
        const unsigned start = 3 + p[2];
        const int substream = p[start];
        if (substream < 0x80 || substream > 0x87 || (audioId >= 0 && audioId != substream))
            return false;
        if (audioId < 0) {
            audioId = substream;
            streamIndex = videoId >= 0 ? 1 : 0;
        }
        const unsigned declaredStarts = p[start + 1], firstOffset = big16(p + start + 2);
        const Byte* payload = p + start + 4;
        const unsigned length = n - start - 4;
        unsigned starts = 0;
        for (unsigned pos = 0; pos < length;) {
            if (!used) {
                if (!starts && firstOffset != pos + 1) return false;
                ++starts;
            }
            const unsigned count = (std::min)(needed - used, length - pos);
            std::memcpy(frame.data() + used, payload + pos, count);
            used += count;
            pos += count;
            if (used != needed) continue;
            if (needed == 7) {
                std::uint8_t bsid = 0;
                std::uint16_t bytes = 0;
                if (av_ac3_parse_header(frame.data(), 7, &bsid, &bytes) < 0 || bsid > 8 ||
                    bytes <= 7 || bytes > 3840) return false;
                const unsigned sr = frame[4] >> 6, mode = frame[6] >> 5;
                constexpr int rates[]{48000, 44100, 32000};
                if (sr >= 3 || (mode != 1 && mode != 2) ||
                    (frame[6] & (mode == 1 ? 0x10 : 0x04))) return false;
                if (rate && (rates[sr] != rate || static_cast<int>(mode) != channels)) return false;
                rate = rates[sr];
                channels = static_cast<int>(mode);
                needed = bytes;
            } else {
                const unsigned split = ((needed / 4) + (needed / 16)) * 2;
                if (av_crc(crc, 0, frame.data() + 2, split - 2) != 0 ||
                    av_crc(crc, 0, frame.data() + split, needed - split) != 0 || frames > UINT64_MAX - 1536)
                    return false;
                frames += 1536;
                used = 0;
                needed = 7;
            }
        }
        return starts == declaredStarts;
    }

    bool sector(const Byte* p) {
        if (std::memcmp(p, "\x00\x00\x01\xba", 4) ||
            (p[4] & 0xc4) != 0x44 || (p[6] & 4) == 0 || (p[8] & 4) == 0 ||
            (p[9] & 1) == 0 || (p[12] & 3) != 3 || (p[13] & 0xf8) != 0xf8)
            return false;
        unsigned pos = 14 + (p[13] & 7);
        while (pos < sectorBytes) {
            if (sectorBytes - pos < 6 || std::memcmp(p + pos, "\x00\x00\x01", 3)) return false;
            const int id = p[pos + 3];
            const unsigned size = big16(p + pos + 4);
            pos += 6;
            if (!size || size > sectorBytes - pos) return false;
            if (id == 0xbd) {
                if (!audioPes(p + pos, size)) return false;
            } else if (id >= 0xe0 && id <= 0xef) {
                if (videoId >= 0 && videoId != id) return false;
                videoId = id;
            } else if (id != 0xbb && id != 0xbe) {
                return false;
            }
            pos += size;
        }
        return true;
    }
};

struct SequentialReader {
    HANDLE file;
    explicit SequentialReader(HANDLE pin)
        : file(ReOpenFile(pin, GENERIC_READ, FILE_SHARE_READ, FILE_FLAG_SEQUENTIAL_SCAN)) {}
    SequentialReader(const SequentialReader&) = delete;
    SequentialReader& operator=(const SequentialReader&) = delete;
    ~SequentialReader() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
};
}

extern "C" AMBM_API int __cdecl AveMediaBridge_InspectVobAc3SourceV1(
    const wchar_t* path, void* pinned, AMBM_Cancel cancel, void* context, Byte* sha256, Count* bytes,
    Count* frames, std::int32_t* stream, std::int32_t* rate, std::int32_t* channels) {
    if (!path || !*path || !pinned || pinned == INVALID_HANDLE_VALUE || !sha256 || !bytes ||
        !frames || !stream || !rate || !channels) return -2;
    try {
        LARGE_INTEGER length{};
        if (GetFileType(pinned) != FILE_TYPE_DISK || !GetFileSizeEx(pinned, &length) || length.QuadPart < 0)
            return -2;
        const Count size = length.QuadPart;
        if (!size || size % sectorBytes) return 0;
        AveMediaBridge::Probe::QueuedSourceSha256 hash(cancel, context);
        if (hash.canceled()) return -1;
        SequentialReader reader(pinned);
        if (reader.file == INVALID_HANDLE_VALUE || !hash.start()) return -2;
        std::vector<Byte> block(1024 * 1024);
        DvdAudio track;
        bool qualified = true;
        for (Count offset = 0; offset < size;) {
            if (hash.canceled()) return -1;
            const auto count = static_cast<DWORD>(std::min<Count>(block.size(), size - offset));
            DWORD got = 0;
            if (!ReadFile(reader.file, block.data(), count, &got, nullptr) || got != count) return -2;
            if (!hash.append(block.data(), count)) return hash.canceled() ? -1 : -2;
            for (unsigned i = 0; qualified && i < count; i += sectorBytes)
                qualified = track.sector(block.data() + i);
            offset += count;
        }
        std::array<Byte, 32> digest{};
        if (!hash.finish(digest, size)) return hash.canceled() ? -1 : -2;
        if (hash.canceled()) return -1;
        std::copy(digest.begin(), digest.end(), sha256);
        *bytes = size;
        if (!qualified || track.used != 0 || track.videoId < 0 || !track.frames) return 2;
        *frames = track.frames;
        *stream = track.streamIndex;
        *rate = track.rate;
        *channels = track.channels;
        return 1;
    } catch (...) {
        return -2;
    }
}
