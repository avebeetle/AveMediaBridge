#pragma once

extern "C" {
#include <libavcodec/ac3_parser.h>
#include <libavformat/avformat.h>
#include <libavutil/crc.h>
}
#include <array>
#include <cstring>

namespace AveMediaBridge::Decode {

inline bool dvdAc3Track(const AVFormatContext* format, const AVStream* stream) {
    return format->iformat && std::strcmp(format->iformat->name, "mpeg") == 0 &&
        stream->id >= 0x80 && stream->id <= 0x87 && stream->codecpar->codec_id == AV_CODEC_ID_AC3;
}

inline bool ac3CrcValid(const uint8_t* data, unsigned size) {
    const auto* crc = av_crc_get_table(AV_CRC_16_ANSI);
    const unsigned split = ((size / 4) + (size / 16)) * 2;
    return av_crc(crc, 0, data + 2, split - 2) == 0 &&
        av_crc(crc, 0, data + split, size - split) == 0;
}

inline unsigned ac3FrameSize(const uint8_t* data, int available) {
    uint8_t bsid = 0;
    uint16_t bytes = 0;
    if (available < 7 || av_ac3_parse_header(data, 7, &bsid, &bytes) < 0 ||
        bsid > 10 || bytes <= 7 || bytes > 3840 || bytes > available) return 0;
    return bytes;
}

inline unsigned verifiedLittleEndianAc3Frame(const uint8_t* source, int available) {
    if (available < 8) return 0;
    std::array<uint8_t, 3840 + AV_INPUT_BUFFER_PADDING_SIZE> normalized{};
    const auto swapWord = [&](unsigned i) {
        normalized[i] = source[i+1];
        normalized[i+1] = source[i];
    };
    for (unsigned i = 0; i < 8; i += 2) swapWord(i);
    const auto size = ac3FrameSize(normalized.data(), available);
    if (!size || (size & 1)) return 0;
    for (unsigned i = 8; i < size; i += 2) swapWord(i);
    return ac3CrcValid(normalized.data(), size) ? size : 0;
}

// MPEG-PS demuxing normally splits AC-3 into syncframes. The AC-3 decoder can
// conceal a truncated syncframe and still return its full 1536 output samples.
inline bool completeDvdAc3Packet(const AVPacket* packet) {
    if (!packet->data || packet->size <= 0 || (packet->flags & AV_PKT_FLAG_CORRUPT)) return false;
    const auto* data = packet->data;
    for (int remaining = packet->size; remaining > 0;) {
        unsigned bytes = 0;
        if (remaining >= 2 && data[0] == 0x77 && data[1] == 0x0b) {
            bytes = verifiedLittleEndianAc3Frame(data, remaining);
            if (!bytes) return false;
        } else {
            bytes = ac3FrameSize(data, remaining);
            if (!bytes || !ac3CrcValid(data, bytes)) return false;
        }
        data += bytes;
        remaining -= bytes;
    }
    return true;
}

// A partial final DVD sector can end before FFmpeg recognizes the next
// syncframe. Only report truncation if its own PES header identifies the
// selected substream and declares bytes beyond the physical end of the input.
inline bool selectedDvdPesTruncated(AVIOContext* io, int selectedId) {
    constexpr int sector = 2048;
    if (!io || !(io->seekable & AVIO_SEEKABLE_NORMAL)) return false;
    const int64_t size = avio_size(io);
    if (size <= 0) return false;
    const int count = static_cast<int>(size % sector);
    if (count < 14) return false;
    std::array<uint8_t, sector> data{};
    const auto start = size - count;
    if (avio_seek(io, start, SEEK_SET) != start || avio_read(io, data.data(), count) != count)
        return true;
    if (std::memcmp(data.data(), "\x00\x00\x01\xba", 4) ||
        (data[4] & 0xc4) != 0x44 || !(data[6] & 4) || !(data[8] & 4) ||
        !(data[9] & 1) || (data[12] & 3) != 3 || (data[13] & 0xf8) != 0xf8) return false;
    for (int pos = 14 + (data[13] & 7); pos + 6 <= count;) {
        if (std::memcmp(data.data() + pos, "\x00\x00\x01", 3)) return false;
        const auto id = data[pos + 3];
        const int length = (data[pos + 4] << 8) | data[pos + 5];
        pos += 6;
        if (!length || length > sector - pos) return false;
        if (length > count - pos) {
            if (id != 0xbd || count - pos < 4 || (data[pos] & 0xc0) != 0x80) return false;
            const int substream = pos + 3 + data[pos + 2];
            return substream < count && substream + 4 <= pos + length && data[substream] == selectedId;
        }
        pos += length;
    }
    return false;
}
}
