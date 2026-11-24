#include "RiffPcmExtent.hpp"
#include "../Input/DemuxByteSource.hpp"
#include "../Input/StableInputContract.hpp"

extern "C" {
#include <libavutil/intreadwrite.h>
}
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace AveMediaBridge::Probe {
namespace {
struct PcmFormat { unsigned tag, bits; };

PcmFormat waveFormat(AVCodecID codec) {
    switch (codec) {
    case AV_CODEC_ID_PCM_U8:    return {1, 8};
    case AV_CODEC_ID_PCM_S16LE: return {1, 16};
    case AV_CODEC_ID_PCM_S24LE: return {1, 24};
    case AV_CODEC_ID_PCM_S32LE: return {1, 32};
    case AV_CODEC_ID_PCM_F32LE: return {3, 32};
    case AV_CODEC_ID_PCM_F64LE: return {3, 64};
    default: return {};
    }
}

bool matchesFormat(const std::uint8_t* data, std::uint64_t size, PcmFormat pcm,
                   unsigned rate, unsigned channels, unsigned alignment) {
    if (size != 16 && size != 18 && size != 40) return false;
    if (AV_RL16(data + 2) != channels || AV_RL32(data + 4) != rate ||
        AV_RL32(data + 8) != static_cast<std::uint64_t>(rate) * alignment ||
        AV_RL16(data + 12) != alignment || AV_RL16(data + 14) != pcm.bits) return false;
    if (size != 40)
        return AV_RL16(data) == pcm.tag && (size == 16 || AV_RL16(data + 16) == 0);

    // WAVEFORMATEXTENSIBLE. The valid bits and subformat must describe the decoded layout.
    static constexpr std::uint8_t guidTail[]{0,0,16,0,128,0,0,170,0,56,155,113};
    const auto mask = AV_RL32(data + 20);
    return AV_RL16(data) == 0xfffe && AV_RL16(data + 16) == 22 &&
        AV_RL16(data + 18) == pcm.bits && (mask == 0 || mask == (channels == 1 ? 4u : 3u)) &&
        AV_RL32(data + 24) == pcm.tag && !std::memcmp(data + 28, guidTail, sizeof guidTail);
}

bool matchesLayout(const std::uint8_t* header, std::uint32_t available, std::uint64_t fileSize,
                   PcmFormat pcm, unsigned rate, unsigned channels, std::uint64_t frames) {
    if (std::memcmp(header, "RIFF", 4) || std::memcmp(header + 8, "WAVE", 4) ||
        AV_RL32(header + 4) != fileSize - 8) return false;
    const unsigned alignment = channels * (pcm.bits / 8);
    bool haveFormat = false, haveFact = false;
    for (std::uint64_t pos = 12; pos + 8 <= available; ) {
        const auto* id = header + pos;
        const std::uint64_t size = AV_RL32(id + 4);
        const auto end = pos + 8 + size;
        const auto paddedEnd = end + (size & 1);
        if (paddedEnd > fileSize) return false;
        if (!std::memcmp(id, "data", 4))
            return haveFormat && paddedEnd == fileSize && size % alignment == 0 && size / alignment == frames;
        if (end > available) return false;
        const auto* payload = id + 8;
        if (!std::memcmp(id, "fmt ", 4)) {
            if (haveFormat || !matchesFormat(payload, size, pcm, rate, channels, alignment)) return false;
            haveFormat = true;
        } else if (!std::memcmp(id, "fact", 4)) {
            if (!haveFormat || haveFact || size != 4 || AV_RL32(payload) != frames) return false;
            haveFact = true;
        } else if (!std::memcmp(id, "LIST", 4)) {
            if (size < 4 || std::memcmp(payload, "INFO", 4)) return false;
        } else if (std::memcmp(id, "JUNK", 4)) {
            return false;
        }
        pos = paddedEnd;
    }
    return false;
}
}

bool matchesRiffPcmExtent(const AVFormatContext* format, const AVStream* stream,
                          const TotalPresentationEvidence& evidence) {
    if (!format || !format->iformat || std::strcmp(format->iformat->name, "wav") ||
        format->nb_streams != 1 || !stream || stream->index != 0 || !stream->codecpar ||
        evidence.source != PresentationTotalSource::ExactPcmStreamDuration ||
        evidence.trust != PresentationTotalTrust::SampleExact || !evidence.exactRescale ||
        evidence.conflict || evidence.frames == 0) return false;
    const auto& codec = *stream->codecpar;
    const PcmFormat pcm = waveFormat(codec.codec_id);
    const auto channels = codec.ch_layout.nb_channels;
    if (!pcm.tag || codec.sample_rate <= 0 || (channels != 1 && channels != 2) ||
        codec.block_align != channels * static_cast<int>(pcm.bits / 8)) return false;
    Input::DemuxByteSource input(format->pb);
    if (!input.valid()) return false;
    const auto source = input.source("RIFF waveform header");
    const auto& pinned = *source.stableSource();
    std::array<std::uint8_t, 4096> header{};
    bool matched = false;
    if (pinned.byteSize >= 44 && pinned.byteSize - 8 <= UINT32_MAX) {
        const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(pinned.byteSize, header.size()));
        std::uint32_t actual = 0;
        if (Input::readSource(pinned, 0, header.data(), count, actual) == AMBI_OK && actual == count) {
            matched = matchesLayout(header.data(), actual, pinned.byteSize, pcm, codec.sample_rate, channels, evidence.frames);
        }
    }
    if (!input.restore()) throw std::runtime_error("cannot restore WAV input position after header inspection");
    return matched;
}
}
