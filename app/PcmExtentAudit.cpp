#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "../src/Core/MediaBridgeError.hpp"
#include "../src/Decode/AudioDecodeHelpers.hpp"
#include "../src/Decode/PcmFormat.hpp"
#include "../src/Ffmpeg/FfmpegDeleters.hpp"
#include "../src/Ffmpeg/FfmpegStreamSelection.hpp"
#include "../src/Probe/MediaProbeService.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {

namespace Decode = AveMediaBridge::Decode;
namespace Ffmpeg = AveMediaBridge::Ffmpeg;
namespace Probe = AveMediaBridge::Probe;

constexpr const char* kAuditEnvVar = "AVEMEDIABRIDGE_PCM_EXTENT_AUDIT";
constexpr const char* kAuditPrefix = "[AVEMEDIABRIDGE_PCM_EXTENT_AUDIT]";
constexpr std::int64_t kFastProbeSizeBytes = 4 * 1024 * 1024;
constexpr std::int64_t kFastProbeAnalyzeDurationUs = 3 * AV_TIME_BASE;

struct Args {
    bool selfTest = false;
    std::filesystem::path input;
    std::filesystem::path outputDir;
};

struct FrameCandidates {
    std::int64_t floor = 0;
    std::int64_t nearest = 0;
    std::int64_t ceil = 0;
    std::int64_t productionLlround = 0;
};

struct PacketEndpoint {
    bool present = false;
    std::int64_t index = 0;
    std::int64_t pts = AV_NOPTS_VALUE;
    std::int64_t dts = AV_NOPTS_VALUE;
    std::int64_t duration = 0;
    std::int64_t pos = -1;
    int size = 0;
    int flags = 0;
};

struct MetadataSnapshot {
    bool ok = false;
    std::string error;
    bool bounded = false;
    std::string formatName;
    std::string formatLongName;
    std::int64_t formatDurationUs = AV_NOPTS_VALUE;
    std::int64_t formatStartTimeUs = AV_NOPTS_VALUE;
    int probeScore = -1;
    int streamCount = 0;
    int selectedAudioStreamIndex = -1;
    std::string codecName;
    int codecId = 0;
    int sampleRate = 0;
    int channels = 0;
    std::string sampleFormat;
    std::string channelLayout;
    std::int64_t bitRate = 0;
    std::int64_t streamDurationTs = AV_NOPTS_VALUE;
    std::int64_t streamStartTimeTs = AV_NOPTS_VALUE;
    AVRational streamTimeBase{0, 1};
    std::int64_t codecDelay = 0;
    std::int64_t initialPadding = 0;
    std::int64_t trailingPadding = 0;
    FrameCandidates formatCandidates;
    FrameCandidates streamCandidates;
};

struct PacketScanSummary {
    bool ok = false;
    std::string error;
    std::int64_t packetCount = 0;
    std::int64_t audioPacketCount = 0;
    std::int64_t packetsWithDuration = 0;
    std::int64_t packetsWithTimestamp = 0;
    std::int64_t packetsWithSkipSamplesSideData = 0;
    std::int64_t skipSamplesAtStart = 0;
    std::int64_t discardPaddingAtEnd = 0;
    long double packetDurationFrameSumExact = 0.0L;
    std::int64_t packetDurationSumFrames = 0;
    std::int64_t firstPacketPts = AV_NOPTS_VALUE;
    std::int64_t lastPacketPts = AV_NOPTS_VALUE;
    std::int64_t lastPacketDuration = 0;
    std::int64_t lastPacketEndPts = AV_NOPTS_VALUE;
    std::int64_t packetPtsSpanFrames = 0;
    std::int64_t packetPtsSpanPlusLastDurationFrames = 0;
    bool packetPtsMonotonic = true;
    PacketEndpoint firstAudioPacket;
    PacketEndpoint lastAudioPacket;
};

struct OggPageSummary {
    bool ok = false;
    std::string error;
    std::int64_t pageCount = 0;
    std::int64_t bosPageCount = 0;
    std::int64_t eosPageCount = 0;
    std::int64_t continuedPageCount = 0;
    std::uint32_t serialNumberCount = 0;
    bool chained = false;
    bool truncated = false;
    std::int64_t firstNonNegativeGranule = -1;
    std::int64_t lastGranulePosition = -1;
    std::int64_t lastEosGranulePosition = -1;
    std::int64_t firstPageOffset = -1;
    std::int64_t lastPageOffset = -1;
};

struct FrameEndpoint {
    bool present = false;
    std::int64_t pts = AV_NOPTS_VALUE;
    int nbSamples = 0;
};

struct DecodeAccounting {
    bool ok = false;
    std::string error;
    std::int64_t packetsRead = 0;
    std::int64_t audioPackets = 0;
    std::int64_t decodedFrameObjectCount = 0;
    std::int64_t sumDecoderNbSamples = 0;
    std::int64_t rawDecoderSampleSum = 0;
    std::int64_t effectiveDecoderSampleSum = 0;
    FrameEndpoint firstFrame;
    FrameEndpoint lastFrame;
    bool swrInitialized = false;
    int inputSampleRate = 0;
    int outputSampleRate = 0;
    int outputChannels = 0;
    std::string inputSampleFormat;
    std::int64_t resamplerInputFrames = 0;
    std::int64_t resamplerOutputFrames = 0;
    std::int64_t resamplerFlushFrames = 0;
    std::int64_t resamplerFlushInvocationCount = 0;
    std::int64_t writerFrames = 0;
    std::int64_t writerBytes = 0;
    std::int64_t writerFrameRemainderBytes = 0;
};

struct SwrAuditState {
    SwrContext* ctx = nullptr;
    AVChannelLayout layout{};
    AVSampleFormat inputFormat = AV_SAMPLE_FMT_NONE;
    int sampleRate = 0;
    int channels = 0;
    bool initialized = false;
    std::vector<float> outputBuffer;
};

struct VersionSnapshot {
    std::string compileTime;
    std::string runtime;
    std::string dllPath;
};

struct AuditResult {
    std::filesystem::path mediaPath;
    std::uintmax_t fileSizeBytes = 0;
    VersionSnapshot avformatVersion;
    VersionSnapshot avcodecVersion;
    VersionSnapshot avutilVersion;
    VersionSnapshot swresampleVersion;
    Probe::FastProbeResult productionFastProbe;
    MetadataSnapshot boundedMetadata;
    MetadataSnapshot fullMetadata;
    PacketScanSummary packetScan;
    OggPageSummary oggPages;
    DecodeAccounting decode;
    std::int64_t initialPublishedFrames = 0;
    std::int64_t aveVoiceAlignedProvisionalFrames = 0;
    std::int64_t differenceAveVoiceProvisionalToFinal = 0;
    std::string initialExtentKind = "unknown";
    std::string initialExtentTrust = "unknown";
    std::string initialExtentSource = "unknown";
    bool initialExtentExact = false;
    std::int64_t finalExactFrames = 0;
    std::int64_t differenceInitialToFinal = 0;
    double differenceSec = 0.0;
    std::string rootCause = "unknown";
    std::string firstMomentExactExtentKnown = "unknown";
    bool exactExtentAvailableAtProbe = false;
    bool exactExtentAvailableBeforeInteractiveLoading = false;
    bool exactExtentAvailableOnlyAfterDecode = false;
};

std::string jsonString(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char ch : value) {
        switch (ch) {
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (ch < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(ch) << std::dec << std::setfill(' ');
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    out << '"';
    return out.str();
}

std::string pathText(const std::filesystem::path& path) {
    return path.string();
}

bool auditEnabled() {
    const char* value = std::getenv(kAuditEnvVar);
    if (!value || value[0] == '\0') {
        return false;
    }
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text != "0" && text != "false" && text != "off" && text != "no";
}

void trace(const std::string& event, const std::string& details) {
    if (!auditEnabled()) {
        return;
    }
    std::cout << kAuditPrefix << " event=" << event;
    if (!details.empty()) {
        std::cout << " " << details;
    }
    std::cout << "\n";
}

std::string ffVersionText(unsigned value) {
    std::ostringstream out;
    out << AV_VERSION_MAJOR(value) << '.'
        << AV_VERSION_MINOR(value) << '.'
        << AV_VERSION_MICRO(value);
    return out.str();
}

#ifdef _WIN32
std::string modulePath(const char* moduleName) {
    HMODULE module = GetModuleHandleA(moduleName);
    if (!module) {
        return {};
    }
    std::array<char, MAX_PATH> buffer{};
    const DWORD copied = GetModuleFileNameA(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (copied == 0 || copied >= buffer.size()) {
        return {};
    }
    return std::string(buffer.data(), copied);
}
#else
std::string modulePath(const char*) {
    return {};
}
#endif

VersionSnapshot makeVersionSnapshot(
    unsigned compileMajor,
    unsigned compileMinor,
    unsigned compileMicro,
    unsigned runtimeVersion,
    const char* dllName) {
    std::ostringstream compile;
    compile << compileMajor << '.' << compileMinor << '.' << compileMicro;
    VersionSnapshot result;
    result.compileTime = compile.str();
    result.runtime = ffVersionText(runtimeVersion);
    result.dllPath = modulePath(dllName);
    return result;
}

std::int64_t rescaleFrames(
    std::int64_t value,
    AVRational sourceTimeBase,
    int sampleRate,
    AVRounding rounding) {
    if (value == AV_NOPTS_VALUE || value <= 0 ||
        sourceTimeBase.num <= 0 || sourceTimeBase.den <= 0 || sampleRate <= 0) {
        return 0;
    }
    const AVRational frameTimeBase{1, sampleRate};
    return av_rescale_q_rnd(
        value,
        sourceTimeBase,
        frameTimeBase,
        static_cast<AVRounding>(rounding | AV_ROUND_PASS_MINMAX));
}

std::int64_t llroundFramesFromSeconds(double seconds, int sampleRate) {
    if (seconds <= 0.0 || sampleRate <= 0 || !std::isfinite(seconds)) {
        return 0;
    }
    return static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(sampleRate)));
}

FrameCandidates makeCandidates(
    std::int64_t value,
    AVRational timeBase,
    int sampleRate) {
    FrameCandidates result;
    result.floor = rescaleFrames(value, timeBase, sampleRate, AV_ROUND_DOWN);
    result.nearest = rescaleFrames(value, timeBase, sampleRate, AV_ROUND_NEAR_INF);
    result.ceil = rescaleFrames(value, timeBase, sampleRate, AV_ROUND_UP);
    if (value != AV_NOPTS_VALUE && timeBase.num > 0 && timeBase.den > 0 && sampleRate > 0) {
        const double seconds = static_cast<double>(value) * av_q2d(timeBase);
        result.productionLlround = llroundFramesFromSeconds(seconds, sampleRate);
    }
    return result;
}

std::string rationalText(AVRational value) {
    std::ostringstream out;
    out << value.num << "/" << value.den;
    return out.str();
}

std::string channelLayoutText(const AVChannelLayout& layout) {
    if (layout.nb_channels <= 0 || !av_channel_layout_check(&layout)) {
        return {};
    }
    char buffer[128] = {};
    if (av_channel_layout_describe(&layout, buffer, sizeof(buffer)) <= 0) {
        return {};
    }
    return std::string(buffer);
}

std::uint32_t readLeU32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) |
        (static_cast<std::uint32_t>(data[1]) << 8) |
        (static_cast<std::uint32_t>(data[2]) << 16) |
        (static_cast<std::uint32_t>(data[3]) << 24);
}

std::int64_t readLeI64(const char* data) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[i])) << (8 * i);
    }
    return static_cast<std::int64_t>(value);
}

void recordSkipSamples(const AVPacket* packet, PacketScanSummary& result) {
    if (!packet) {
        return;
    }
    size_t sideDataSize = 0;
    const std::uint8_t* sideData =
        av_packet_get_side_data(packet, AV_PKT_DATA_SKIP_SAMPLES, &sideDataSize);
    if (!sideData || sideDataSize < 10) {
        return;
    }
    ++result.packetsWithSkipSamplesSideData;
    result.skipSamplesAtStart += static_cast<std::int64_t>(readLeU32(sideData));
    result.discardPaddingAtEnd += static_cast<std::int64_t>(readLeU32(sideData + 4));
}

PacketEndpoint makePacketEndpoint(std::int64_t index, const AVPacket* packet) {
    PacketEndpoint result;
    result.present = packet != nullptr;
    result.index = index;
    if (!packet) {
        return result;
    }
    result.pts = packet->pts;
    result.dts = packet->dts;
    result.duration = packet->duration;
    result.pos = packet->pos;
    result.size = packet->size;
    result.flags = packet->flags;
    return result;
}

MetadataSnapshot inspectMetadata(const std::string& path, bool bounded) {
    MetadataSnapshot result;
    result.bounded = bounded;

    Ffmpeg::UniqueAVFormatContext formatContext(avformat_alloc_context());
    if (!formatContext) {
        result.error = "avformat_alloc_context failed";
        return result;
    }
    AVDictionary* options = nullptr;
    if (bounded) {
        formatContext->probesize = kFastProbeSizeBytes;
        formatContext->max_analyze_duration = kFastProbeAnalyzeDurationUs;
        av_dict_set(&options, "probesize", std::to_string(kFastProbeSizeBytes).c_str(), 0);
        av_dict_set(&options, "analyzeduration", std::to_string(kFastProbeAnalyzeDurationUs).c_str(), 0);
    }

    AVFormatContext* rawContext = formatContext.release();
    int ret = avformat_open_input(&rawContext, path.c_str(), nullptr, &options);
    formatContext.reset(rawContext);
    av_dict_free(&options);
    if (ret < 0) {
        result.error = "avformat_open_input failed: " + AveMediaBridge::ffErrorString(ret);
        return result;
    }
    ret = avformat_find_stream_info(formatContext.get(), nullptr);
    if (ret < 0) {
        result.error = "avformat_find_stream_info failed: " + AveMediaBridge::ffErrorString(ret);
        return result;
    }

    result.streamCount = static_cast<int>(formatContext->nb_streams);
    result.formatDurationUs = formatContext->duration;
    result.formatStartTimeUs = formatContext->start_time;
    result.probeScore = formatContext->probe_score;
    if (formatContext->iformat) {
        result.formatName = formatContext->iformat->name ? formatContext->iformat->name : "";
        result.formatLongName = formatContext->iformat->long_name ? formatContext->iformat->long_name : "";
    }

    const Ffmpeg::AudioStreamSelection selection =
        Ffmpeg::selectBestAudioStreamWithFirstAudioFallback(formatContext.get());
    if (selection.streamIndex < 0 ||
        selection.streamIndex >= static_cast<int>(formatContext->nb_streams)) {
        result.error = "no audio stream found";
        return result;
    }

    result.selectedAudioStreamIndex = selection.streamIndex;
    const AVStream* stream = formatContext->streams[selection.streamIndex];
    const AVCodecParameters* codecpar = stream ? stream->codecpar : nullptr;
    if (!stream || !codecpar) {
        result.error = "selected stream has no codec parameters";
        return result;
    }

    result.codecName = avcodec_get_name(codecpar->codec_id);
    result.codecId = static_cast<int>(codecpar->codec_id);
    result.sampleRate = codecpar->sample_rate;
    result.channels = codecpar->ch_layout.nb_channels;
    result.channelLayout = channelLayoutText(codecpar->ch_layout);
    result.bitRate = codecpar->bit_rate;
    result.streamDurationTs = stream->duration;
    result.streamStartTimeTs = stream->start_time;
    result.streamTimeBase = stream->time_base;
    result.initialPadding = codecpar->initial_padding > 0 ? codecpar->initial_padding : 0;
    result.trailingPadding = codecpar->trailing_padding > 0 ? codecpar->trailing_padding : 0;
    result.formatCandidates = makeCandidates(
        result.formatDurationUs,
        AVRational{1, AV_TIME_BASE},
        result.sampleRate);
    result.streamCandidates = makeCandidates(
        result.streamDurationTs,
        result.streamTimeBase,
        result.sampleRate);

    const AVCodec* decoder = selection.decoder ? selection.decoder : avcodec_find_decoder(codecpar->codec_id);
    if (decoder) {
        Ffmpeg::CodecContextDeleter codecContextDeleter;
        AVCodecContext* decoderContext = avcodec_alloc_context3(decoder);
        std::unique_ptr<AVCodecContext, Ffmpeg::CodecContextDeleter> decoderGuard(decoderContext);
        if (decoderContext && avcodec_parameters_to_context(decoderContext, codecpar) >= 0) {
            result.codecDelay = decoderContext->delay > 0 ? decoderContext->delay : 0;
            result.initialPadding =
                std::max<std::int64_t>(result.initialPadding, decoderContext->initial_padding);
            result.trailingPadding =
                std::max<std::int64_t>(result.trailingPadding, decoderContext->trailing_padding);
        }
    }

    result.ok = true;
    return result;
}

PacketScanSummary inspectPackets(const std::string& path, int audioStreamIndex, int sampleRate) {
    PacketScanSummary result;
    if (audioStreamIndex < 0 || sampleRate <= 0) {
        result.error = "invalid audio stream or sample rate";
        return result;
    }

    Ffmpeg::UniqueAVFormatContext formatContext(avformat_alloc_context());
    if (!formatContext) {
        result.error = "avformat_alloc_context failed";
        return result;
    }

    AVFormatContext* rawContext = formatContext.release();
    int ret = avformat_open_input(&rawContext, path.c_str(), nullptr, nullptr);
    formatContext.reset(rawContext);
    if (ret < 0) {
        result.error = "avformat_open_input failed: " + AveMediaBridge::ffErrorString(ret);
        return result;
    }
    ret = avformat_find_stream_info(formatContext.get(), nullptr);
    if (ret < 0) {
        result.error = "avformat_find_stream_info failed: " + AveMediaBridge::ffErrorString(ret);
        return result;
    }
    if (audioStreamIndex >= static_cast<int>(formatContext->nb_streams) ||
        !formatContext->streams[audioStreamIndex]) {
        result.error = "audio stream index mismatch";
        return result;
    }

    const AVStream* stream = formatContext->streams[audioStreamIndex];
    const AVRational timeBase = stream->time_base;
    Ffmpeg::UniqueAVPacket packet(av_packet_alloc());
    if (!packet) {
        result.error = "av_packet_alloc failed";
        return result;
    }

    std::int64_t previousPacketPts = AV_NOPTS_VALUE;
    while ((ret = av_read_frame(formatContext.get(), packet.get())) >= 0) {
        ++result.packetCount;
        if (packet->stream_index == audioStreamIndex) {
            ++result.audioPacketCount;
            if (!result.firstAudioPacket.present) {
                result.firstAudioPacket = makePacketEndpoint(result.audioPacketCount, packet.get());
            }
            result.lastAudioPacket = makePacketEndpoint(result.audioPacketCount, packet.get());
            if (packet->duration > 0) {
                ++result.packetsWithDuration;
                result.lastPacketDuration = packet->duration;
                result.packetDurationFrameSumExact +=
                    static_cast<long double>(packet->duration) *
                    static_cast<long double>(timeBase.num) *
                    static_cast<long double>(sampleRate) /
                    static_cast<long double>(timeBase.den);
            }
            const std::int64_t pts =
                packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
            if (pts != AV_NOPTS_VALUE) {
                ++result.packetsWithTimestamp;
                if (previousPacketPts != AV_NOPTS_VALUE && pts < previousPacketPts) {
                    result.packetPtsMonotonic = false;
                }
                previousPacketPts = pts;
                if (result.firstPacketPts == AV_NOPTS_VALUE || pts < result.firstPacketPts) {
                    result.firstPacketPts = pts;
                }
                if (result.lastPacketPts == AV_NOPTS_VALUE || pts >= result.lastPacketPts) {
                    result.lastPacketPts = pts;
                    result.lastPacketEndPts = packet->duration > 0 ? pts + packet->duration : pts;
                }
            }
            recordSkipSamples(packet.get(), result);
        }
        av_packet_unref(packet.get());
    }

    if (ret != AVERROR_EOF) {
        result.error = "av_read_frame failed: " + AveMediaBridge::ffErrorString(ret);
        return result;
    }

    result.packetDurationSumFrames =
        static_cast<std::int64_t>(std::llround(result.packetDurationFrameSumExact));
    if (result.firstPacketPts != AV_NOPTS_VALUE &&
        result.lastPacketPts != AV_NOPTS_VALUE &&
        result.lastPacketPts > result.firstPacketPts) {
        result.packetPtsSpanFrames = rescaleFrames(
            result.lastPacketPts - result.firstPacketPts,
            timeBase,
            sampleRate,
            AV_ROUND_NEAR_INF);
    }
    if (result.firstPacketPts != AV_NOPTS_VALUE &&
        result.lastPacketEndPts != AV_NOPTS_VALUE &&
        result.lastPacketEndPts > result.firstPacketPts) {
        result.packetPtsSpanPlusLastDurationFrames = rescaleFrames(
            result.lastPacketEndPts - result.firstPacketPts,
            timeBase,
            sampleRate,
            AV_ROUND_NEAR_INF);
    }
    result.ok = true;
    return result;
}

OggPageSummary inspectOggPages(const std::filesystem::path& path) {
    OggPageSummary result;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.error = "failed to open input for Ogg page scan";
        return result;
    }

    std::set<std::uint32_t> serials;
    while (true) {
        const std::streamoff pageOffset = input.tellg();
        std::array<char, 27> header{};
        input.read(header.data(), static_cast<std::streamsize>(header.size()));
        if (input.gcount() == 0 && input.eof()) {
            break;
        }
        if (input.gcount() != static_cast<std::streamsize>(header.size())) {
            result.truncated = true;
            result.error = "truncated Ogg page header";
            break;
        }
        if (std::string(header.data(), 4) != "OggS") {
            result.error = "invalid Ogg capture pattern";
            break;
        }
        const unsigned char headerType = static_cast<unsigned char>(header[5]);
        const std::int64_t granule = readLeI64(header.data() + 6);
        const std::uint32_t serial =
            static_cast<std::uint32_t>(static_cast<unsigned char>(header[14])) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(header[15])) << 8) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(header[16])) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(header[17])) << 24);
        const int pageSegments = static_cast<unsigned char>(header[26]);
        std::vector<unsigned char> lacing(static_cast<std::size_t>(pageSegments));
        if (!lacing.empty()) {
            input.read(reinterpret_cast<char*>(lacing.data()), static_cast<std::streamsize>(lacing.size()));
            if (input.gcount() != static_cast<std::streamsize>(lacing.size())) {
                result.truncated = true;
                result.error = "truncated Ogg segment table";
                break;
            }
        }
        std::int64_t payloadBytes = 0;
        for (unsigned char lace : lacing) {
            payloadBytes += lace;
        }
        input.seekg(payloadBytes, std::ios::cur);
        if (!input) {
            result.truncated = true;
            result.error = "truncated Ogg page payload";
            break;
        }

        ++result.pageCount;
        if (result.firstPageOffset < 0) {
            result.firstPageOffset = static_cast<std::int64_t>(pageOffset);
        }
        result.lastPageOffset = static_cast<std::int64_t>(pageOffset);
        serials.insert(serial);
        if ((headerType & 0x01u) != 0) {
            ++result.continuedPageCount;
        }
        if ((headerType & 0x02u) != 0) {
            ++result.bosPageCount;
        }
        if ((headerType & 0x04u) != 0) {
            ++result.eosPageCount;
            result.lastEosGranulePosition = granule;
        }
        if (granule >= 0) {
            if (result.firstNonNegativeGranule < 0) {
                result.firstNonNegativeGranule = granule;
            }
            result.lastGranulePosition = granule;
        }
    }

    result.serialNumberCount = static_cast<std::uint32_t>(serials.size());
    result.chained = result.serialNumberCount > 1;
    result.ok = result.error.empty();
    return result;
}

int ensureSwr(SwrAuditState& swr, const AVFrame* frame, const AVCodecContext* decoder, DecodeAccounting& result) {
    if (swr.initialized) {
        return 0;
    }
    const auto inputFormat = static_cast<AVSampleFormat>(frame->format);
    if (inputFormat == AV_SAMPLE_FMT_NONE) {
        result.error = "decoded frame has unknown sample format";
        return AVERROR(EINVAL);
    }
    const int sampleRate = frame->sample_rate > 0 ? frame->sample_rate : decoder->sample_rate;
    if (sampleRate <= 0) {
        result.error = "decoded frame has invalid sample rate";
        return AVERROR(EINVAL);
    }

    AVChannelLayout inputLayout{};
    int ret = Decode::copyDecodedFrameLayout(&inputLayout, frame, decoder);
    if (ret < 0) {
        result.error = "unable to determine decoded channel layout: " + AveMediaBridge::ffErrorString(ret);
        return ret;
    }

    ret = av_channel_layout_copy(&swr.layout, &inputLayout);
    if (ret < 0) {
        av_channel_layout_uninit(&inputLayout);
        result.error = "unable to copy output channel layout: " + AveMediaBridge::ffErrorString(ret);
        return ret;
    }
    swr.sampleRate = sampleRate;
    swr.channels = swr.layout.nb_channels;
    swr.inputFormat = inputFormat;

    ret = swr_alloc_set_opts2(
        &swr.ctx,
        &swr.layout,
        AV_SAMPLE_FMT_FLT,
        swr.sampleRate,
        &inputLayout,
        inputFormat,
        sampleRate,
        0,
        nullptr);
    av_channel_layout_uninit(&inputLayout);
    if (ret < 0) {
        result.error = "swr_alloc_set_opts2 failed: " + AveMediaBridge::ffErrorString(ret);
        return ret;
    }
    ret = swr_init(swr.ctx);
    if (ret < 0) {
        result.error = "swr_init failed: " + AveMediaBridge::ffErrorString(ret);
        return ret;
    }
    swr.initialized = true;
    result.swrInitialized = true;
    result.inputSampleRate = sampleRate;
    result.outputSampleRate = sampleRate;
    result.outputChannels = swr.channels;
    result.inputSampleFormat = Decode::sampleFormatName(inputFormat);
    return 0;
}

int convertFrame(SwrAuditState& swr, const AVFrame* frame, const AVCodecContext* decoder, DecodeAccounting& result) {
    int ret = ensureSwr(swr, frame, decoder, result);
    if (ret < 0) {
        return ret;
    }
    result.resamplerInputFrames += frame->nb_samples;
    const int outCapacity = swr_get_out_samples(swr.ctx, frame->nb_samples);
    if (outCapacity < 0) {
        result.error = "swr_get_out_samples failed: " + AveMediaBridge::ffErrorString(outCapacity);
        return outCapacity;
    }
    if (outCapacity == 0) {
        return 0;
    }
    const std::size_t sampleCapacity =
        static_cast<std::size_t>(outCapacity) * static_cast<std::size_t>(swr.channels);
    if (swr.outputBuffer.size() < sampleCapacity) {
        swr.outputBuffer.resize(sampleCapacity);
    }
    uint8_t* outData[] = { reinterpret_cast<uint8_t*>(swr.outputBuffer.data()) };
    const uint8_t* const* inData = const_cast<const uint8_t* const*>(frame->extended_data);
    ret = swr_convert(swr.ctx, outData, outCapacity, inData, frame->nb_samples);
    if (ret < 0) {
        result.error = "swr_convert failed: " + AveMediaBridge::ffErrorString(ret);
        return ret;
    }
    result.resamplerOutputFrames += ret;
    result.writerFrames += ret;
    result.writerBytes +=
        static_cast<std::int64_t>(ret) *
        static_cast<std::int64_t>(swr.channels) *
        static_cast<std::int64_t>(sizeof(float));
    return 0;
}

int flushSwr(SwrAuditState& swr, DecodeAccounting& result) {
    if (!swr.initialized) {
        return 0;
    }
    while (true) {
        const std::int64_t delay = swr_get_delay(swr.ctx, swr.sampleRate);
        if (delay <= 0) {
            break;
        }
        const int outCapacity =
            delay > static_cast<std::int64_t>((std::numeric_limits<int>::max)())
                ? (std::numeric_limits<int>::max)()
                : static_cast<int>(delay);
        const std::size_t sampleCapacity =
            static_cast<std::size_t>(outCapacity) * static_cast<std::size_t>(swr.channels);
        if (swr.outputBuffer.size() < sampleCapacity) {
            swr.outputBuffer.resize(sampleCapacity);
        }
        uint8_t* outData[] = { reinterpret_cast<uint8_t*>(swr.outputBuffer.data()) };
        const int ret = swr_convert(swr.ctx, outData, outCapacity, nullptr, 0);
        ++result.resamplerFlushInvocationCount;
        if (ret < 0) {
            result.error = "swr_convert flush failed: " + AveMediaBridge::ffErrorString(ret);
            return ret;
        }
        if (ret == 0) {
            break;
        }
        result.resamplerOutputFrames += ret;
        result.resamplerFlushFrames += ret;
        result.writerFrames += ret;
        result.writerBytes +=
            static_cast<std::int64_t>(ret) *
            static_cast<std::int64_t>(swr.channels) *
            static_cast<std::int64_t>(sizeof(float));
    }
    return 0;
}

int receiveFrames(AVCodecContext* decoder, AVFrame* frame, SwrAuditState& swr, DecodeAccounting& result) {
    while (true) {
        const int ret = avcodec_receive_frame(decoder, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return 0;
        }
        if (ret < 0) {
            result.error = "avcodec_receive_frame failed: " + AveMediaBridge::ffErrorString(ret);
            return ret;
        }
        ++result.decodedFrameObjectCount;
        result.sumDecoderNbSamples += frame->nb_samples;
        result.rawDecoderSampleSum += frame->nb_samples;
        result.effectiveDecoderSampleSum += frame->nb_samples;
        if (!result.firstFrame.present) {
            result.firstFrame.present = true;
            result.firstFrame.pts = frame->pts;
            result.firstFrame.nbSamples = frame->nb_samples;
        }
        result.lastFrame.present = true;
        result.lastFrame.pts = frame->pts;
        result.lastFrame.nbSamples = frame->nb_samples;
        const int convertRet = convertFrame(swr, frame, decoder, result);
        av_frame_unref(frame);
        if (convertRet < 0) {
            return convertRet;
        }
    }
}

DecodeAccounting decodeCount(const std::string& path) {
    DecodeAccounting result;
    Ffmpeg::UniqueAVFormatContext formatContext(avformat_alloc_context());
    if (!formatContext) {
        result.error = "avformat_alloc_context failed";
        return result;
    }
    AVFormatContext* rawContext = formatContext.release();
    int ret = avformat_open_input(&rawContext, path.c_str(), nullptr, nullptr);
    formatContext.reset(rawContext);
    if (ret < 0) {
        result.error = "avformat_open_input failed: " + AveMediaBridge::ffErrorString(ret);
        return result;
    }
    ret = avformat_find_stream_info(formatContext.get(), nullptr);
    if (ret < 0) {
        result.error = "avformat_find_stream_info failed: " + AveMediaBridge::ffErrorString(ret);
        return result;
    }

    const Ffmpeg::AudioStreamSelection selection =
        Ffmpeg::selectBestAudioStreamStrict(formatContext.get());
    if (selection.streamIndex < 0) {
        result.error = "no audio stream found: " + AveMediaBridge::ffErrorString(selection.bestStreamResult);
        return result;
    }
    AVStream* stream = formatContext->streams[selection.streamIndex];
    AVCodecParameters* codecpar = stream ? stream->codecpar : nullptr;
    const AVCodec* decoder = selection.decoder;
    if (!decoder && codecpar) {
        decoder = avcodec_find_decoder(codecpar->codec_id);
    }
    if (!decoder || !codecpar) {
        result.error = "decoder not found";
        return result;
    }

    AVCodecContext* decoderContextRaw = avcodec_alloc_context3(decoder);
    std::unique_ptr<AVCodecContext, Ffmpeg::CodecContextDeleter> decoderContext(decoderContextRaw);
    if (!decoderContext) {
        result.error = "avcodec_alloc_context3 failed";
        return result;
    }
    ret = avcodec_parameters_to_context(decoderContext.get(), codecpar);
    if (ret < 0) {
        result.error = "avcodec_parameters_to_context failed: " + AveMediaBridge::ffErrorString(ret);
        return result;
    }
    if (decoderContext->ch_layout.nb_channels <= 0 && codecpar->ch_layout.nb_channels > 0) {
        av_channel_layout_copy(&decoderContext->ch_layout, &codecpar->ch_layout);
    }
    ret = avcodec_open2(decoderContext.get(), decoder, nullptr);
    if (ret < 0) {
        result.error = "avcodec_open2 failed: " + AveMediaBridge::ffErrorString(ret);
        return result;
    }

    Ffmpeg::UniqueAVPacket packet(av_packet_alloc());
    std::unique_ptr<AVFrame, Ffmpeg::FrameDeleter> frame(av_frame_alloc());
    if (!packet || !frame) {
        result.error = "unable to allocate packet/frame";
        return result;
    }
    SwrAuditState swr;
    auto cleanupSwr = [&]() {
        if (swr.ctx) {
            swr_free(&swr.ctx);
        }
        av_channel_layout_uninit(&swr.layout);
    };

    while ((ret = av_read_frame(formatContext.get(), packet.get())) >= 0) {
        ++result.packetsRead;
        if (packet->stream_index == selection.streamIndex) {
            ++result.audioPackets;
            ret = avcodec_send_packet(decoderContext.get(), packet.get());
            if (ret == AVERROR(EAGAIN)) {
                ret = receiveFrames(decoderContext.get(), frame.get(), swr, result);
                if (ret >= 0) {
                    ret = avcodec_send_packet(decoderContext.get(), packet.get());
                }
            }
            av_packet_unref(packet.get());
            if (ret < 0) {
                result.error = "avcodec_send_packet failed: " + AveMediaBridge::ffErrorString(ret);
                cleanupSwr();
                return result;
            }
            ret = receiveFrames(decoderContext.get(), frame.get(), swr, result);
            if (ret < 0) {
                cleanupSwr();
                return result;
            }
        } else {
            av_packet_unref(packet.get());
        }
    }
    if (ret != AVERROR_EOF) {
        result.error = "av_read_frame failed: " + AveMediaBridge::ffErrorString(ret);
        cleanupSwr();
        return result;
    }

    ret = avcodec_send_packet(decoderContext.get(), nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        result.error = "decoder flush send failed: " + AveMediaBridge::ffErrorString(ret);
        cleanupSwr();
        return result;
    }
    ret = receiveFrames(decoderContext.get(), frame.get(), swr, result);
    if (ret < 0) {
        cleanupSwr();
        return result;
    }
    ret = flushSwr(swr, result);
    cleanupSwr();
    if (ret < 0) {
        return result;
    }

    if (result.outputChannels > 0) {
        const std::int64_t bytesPerFrame =
            static_cast<std::int64_t>(result.outputChannels) * static_cast<std::int64_t>(sizeof(float));
        result.writerFrameRemainderBytes =
            bytesPerFrame > 0 ? result.writerBytes % bytesPerFrame : result.writerBytes;
    }
    result.ok = result.error.empty();
    return result;
}

void writeCandidates(std::ostream& out, const char* name, const FrameCandidates& value, const char* suffix) {
    out << "  \"" << name << "FramesFloor" << suffix << "\": " << value.floor << ",\n";
    out << "  \"" << name << "FramesNearest" << suffix << "\": " << value.nearest << ",\n";
    out << "  \"" << name << "FramesCeil" << suffix << "\": " << value.ceil << ",\n";
    out << "  \"" << name << "FramesProductionLlround" << suffix << "\": " << value.productionLlround;
}

bool ensureDirectory(const std::filesystem::path& path, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        error = "failed to create output directory: " + ec.message();
        return false;
    }
    return true;
}

void writeExtentCandidatesJson(const std::filesystem::path& outputPath, const AuditResult& result) {
    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    out << "{\n";
    out << "  \"mediaPath\": " << jsonString(pathText(result.mediaPath)) << ",\n";
    out << "  \"boundedProbe\": {\n";
    out << "    \"formatDurationUs\": " << result.boundedMetadata.formatDurationUs << ",\n";
    out << "    \"streamDurationTs\": " << result.boundedMetadata.streamDurationTs << ",\n";
    out << "    \"streamTimeBase\": " << jsonString(rationalText(result.boundedMetadata.streamTimeBase)) << ",\n";
    writeCandidates(out, "formatDuration", result.boundedMetadata.formatCandidates, "");
    out << ",\n";
    writeCandidates(out, "streamDuration", result.boundedMetadata.streamCandidates, "");
    out << "\n  },\n";
    out << "  \"fullProbe\": {\n";
    out << "    \"formatDurationUs\": " << result.fullMetadata.formatDurationUs << ",\n";
    out << "    \"streamDurationTs\": " << result.fullMetadata.streamDurationTs << ",\n";
    out << "    \"streamTimeBase\": " << jsonString(rationalText(result.fullMetadata.streamTimeBase)) << ",\n";
    writeCandidates(out, "formatDuration", result.fullMetadata.formatCandidates, "");
    out << ",\n";
    writeCandidates(out, "streamDuration", result.fullMetadata.streamCandidates, "");
    out << "\n  }\n";
    out << "}\n";
}

void writeDecodeAccountingJson(const std::filesystem::path& outputPath, const AuditResult& result) {
    const DecodeAccounting& decode = result.decode;
    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    out << "{\n";
    out << "  \"ok\": " << (decode.ok ? "true" : "false") << ",\n";
    out << "  \"error\": " << jsonString(decode.error) << ",\n";
    out << "  \"packetsRead\": " << decode.packetsRead << ",\n";
    out << "  \"audioPackets\": " << decode.audioPackets << ",\n";
    out << "  \"decodedFrameObjectCount\": " << decode.decodedFrameObjectCount << ",\n";
    out << "  \"sumDecoderNbSamples\": " << decode.sumDecoderNbSamples << ",\n";
    out << "  \"rawDecoderSampleSum\": " << decode.rawDecoderSampleSum << ",\n";
    out << "  \"effectiveDecoderSampleSum\": " << decode.effectiveDecoderSampleSum << ",\n";
    out << "  \"firstFramePts\": " << decode.firstFrame.pts << ",\n";
    out << "  \"firstFrameNbSamples\": " << decode.firstFrame.nbSamples << ",\n";
    out << "  \"lastFramePts\": " << decode.lastFrame.pts << ",\n";
    out << "  \"lastFrameNbSamples\": " << decode.lastFrame.nbSamples << ",\n";
    out << "  \"swrInitialized\": " << (decode.swrInitialized ? "true" : "false") << ",\n";
    out << "  \"inputSampleRate\": " << decode.inputSampleRate << ",\n";
    out << "  \"outputSampleRate\": " << decode.outputSampleRate << ",\n";
    out << "  \"outputChannels\": " << decode.outputChannels << ",\n";
    out << "  \"inputSampleFormat\": " << jsonString(decode.inputSampleFormat) << ",\n";
    out << "  \"resamplerInputFrames\": " << decode.resamplerInputFrames << ",\n";
    out << "  \"resamplerOutputFrames\": " << decode.resamplerOutputFrames << ",\n";
    out << "  \"resamplerFlushFrames\": " << decode.resamplerFlushFrames << ",\n";
    out << "  \"resamplerFlushInvocationCount\": " << decode.resamplerFlushInvocationCount << ",\n";
    out << "  \"writerFrames\": " << decode.writerFrames << ",\n";
    out << "  \"writerBytes\": " << decode.writerBytes << ",\n";
    out << "  \"writerFrameRemainderBytes\": " << decode.writerFrameRemainderBytes << "\n";
    out << "}\n";
}

void writePacketEndpoint(std::ostream& out, const char* name, const PacketEndpoint& endpoint, bool trailingComma) {
    out << "  \"" << name << "\": {\n";
    out << "    \"present\": " << (endpoint.present ? "true" : "false") << ",\n";
    out << "    \"index\": " << endpoint.index << ",\n";
    out << "    \"pts\": " << endpoint.pts << ",\n";
    out << "    \"dts\": " << endpoint.dts << ",\n";
    out << "    \"duration\": " << endpoint.duration << ",\n";
    out << "    \"pos\": " << endpoint.pos << ",\n";
    out << "    \"size\": " << endpoint.size << ",\n";
    out << "    \"flags\": " << endpoint.flags << "\n";
    out << "  }" << (trailingComma ? "," : "") << "\n";
}

void writePacketTimingJsonl(const std::filesystem::path& outputPath, const AuditResult& result) {
    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    const auto eventLine = [&](const std::string& event, const std::string& fields) {
        out << "{\"event\":" << jsonString(event);
        if (!fields.empty()) {
            out << "," << fields;
        }
        out << "}\n";
    };
    eventLine(
        "ProbeOpened",
        "\"boundedStreamDurationTs\":" + std::to_string(result.boundedMetadata.streamDurationTs) +
            ",\"fullStreamDurationTs\":" + std::to_string(result.fullMetadata.streamDurationTs));
    eventLine(
        "InitialExtentCalculated",
        "\"frames\":" + std::to_string(result.initialPublishedFrames) +
            ",\"kind\":" + jsonString(result.initialExtentKind) +
            ",\"source\":" + jsonString(result.initialExtentSource));
    eventLine(
        "InitialExtentPublished",
        "\"frames\":" + std::to_string(result.initialPublishedFrames) +
            ",\"trust\":" + jsonString(result.initialExtentTrust));
    eventLine(
        "FirstPacketObserved",
        "\"pts\":" + std::to_string(result.packetScan.firstAudioPacket.pts) +
            ",\"duration\":" + std::to_string(result.packetScan.firstAudioPacket.duration));
    eventLine(
        "LastPacketObserved",
        "\"pts\":" + std::to_string(result.packetScan.lastAudioPacket.pts) +
            ",\"duration\":" + std::to_string(result.packetScan.lastAudioPacket.duration));
    eventLine(
        "LastGranuleObserved",
        "\"lastGranulePosition\":" + std::to_string(result.oggPages.lastGranulePosition) +
            ",\"lastEosGranulePosition\":" + std::to_string(result.oggPages.lastEosGranulePosition));
    eventLine(
        "SkipSamplesObserved",
        "\"skipSamplesAtStart\":" + std::to_string(result.packetScan.skipSamplesAtStart));
    eventLine(
        "DiscardPaddingObserved",
        "\"discardPaddingAtEnd\":" + std::to_string(result.packetScan.discardPaddingAtEnd));
    eventLine(
        "DecoderCompleted",
        "\"rawDecoderSampleSum\":" + std::to_string(result.decode.rawDecoderSampleSum) +
            ",\"effectiveDecoderSampleSum\":" + std::to_string(result.decode.effectiveDecoderSampleSum));
    eventLine(
        "ResamplerCompleted",
        "\"resamplerInputFrames\":" + std::to_string(result.decode.resamplerInputFrames) +
            ",\"resamplerOutputFrames\":" + std::to_string(result.decode.resamplerOutputFrames) +
            ",\"flushFrames\":" + std::to_string(result.decode.resamplerFlushFrames));
    eventLine(
        "WriterCompleted",
        "\"writerFrames\":" + std::to_string(result.decode.writerFrames) +
            ",\"writerBytes\":" + std::to_string(result.decode.writerBytes));
    eventLine(
        "FinalExactExtentKnown",
        "\"finalExactFrames\":" + std::to_string(result.finalExactFrames) +
            ",\"firstMoment\":" + jsonString(result.firstMomentExactExtentKnown));
    eventLine(
        "ExtentMismatchClassified",
        "\"rootCause\":" + jsonString(result.rootCause) +
            ",\"differenceInitialToFinal\":" + std::to_string(result.differenceInitialToFinal));
}

void writeSummaryJson(const std::filesystem::path& outputPath, const AuditResult& result) {
    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    out << "{\n";
    out << "  \"mediaPath\": " << jsonString(pathText(result.mediaPath)) << ",\n";
    out << "  \"fileSizeBytes\": " << result.fileSizeBytes << ",\n";
    out << "  \"container\": " << jsonString(result.fullMetadata.formatName) << ",\n";
    out << "  \"codec\": " << jsonString(result.fullMetadata.codecName) << ",\n";
    out << "  \"sampleRate\": " << result.fullMetadata.sampleRate << ",\n";
    out << "  \"channels\": " << result.fullMetadata.channels << ",\n";
    out << "  \"linkedFfmpeg\": {\n";
    out << "    \"libavformatCompile\": " << jsonString(result.avformatVersion.compileTime) << ",\n";
    out << "    \"libavformatRuntime\": " << jsonString(result.avformatVersion.runtime) << ",\n";
    out << "    \"libavformatDllPath\": " << jsonString(result.avformatVersion.dllPath) << ",\n";
    out << "    \"libavcodecCompile\": " << jsonString(result.avcodecVersion.compileTime) << ",\n";
    out << "    \"libavcodecRuntime\": " << jsonString(result.avcodecVersion.runtime) << ",\n";
    out << "    \"libavcodecDllPath\": " << jsonString(result.avcodecVersion.dllPath) << ",\n";
    out << "    \"libavutilCompile\": " << jsonString(result.avutilVersion.compileTime) << ",\n";
    out << "    \"libavutilRuntime\": " << jsonString(result.avutilVersion.runtime) << ",\n";
    out << "    \"libavutilDllPath\": " << jsonString(result.avutilVersion.dllPath) << ",\n";
    out << "    \"libswresampleCompile\": " << jsonString(result.swresampleVersion.compileTime) << ",\n";
    out << "    \"libswresampleRuntime\": " << jsonString(result.swresampleVersion.runtime) << ",\n";
    out << "    \"libswresampleDllPath\": " << jsonString(result.swresampleVersion.dllPath) << "\n";
    out << "  },\n";
    out << "  \"formatDurationUs\": " << result.fullMetadata.formatDurationUs << ",\n";
    out << "  \"streamDurationTs\": " << result.fullMetadata.streamDurationTs << ",\n";
    out << "  \"streamTimeBase\": " << jsonString(rationalText(result.fullMetadata.streamTimeBase)) << ",\n";
    out << "  \"initialPublishedFrames\": " << result.initialPublishedFrames << ",\n";
    out << "  \"aveVoiceAlignedProvisionalFrames\": " << result.aveVoiceAlignedProvisionalFrames << ",\n";
    out << "  \"differenceAveVoiceProvisionalToFinal\": "
        << result.differenceAveVoiceProvisionalToFinal << ",\n";
    out << "  \"initialExtentKind\": " << jsonString(result.initialExtentKind) << ",\n";
    out << "  \"initialExtentTrust\": " << jsonString(result.initialExtentTrust) << ",\n";
    out << "  \"initialExtentSource\": " << jsonString(result.initialExtentSource) << ",\n";
    out << "  \"initialExtentExact\": " << (result.initialExtentExact ? "true" : "false") << ",\n";
    out << "  \"initialCalculationFunction\": \"AveMediaBridge::Probe::estimateFastDurationAndFrames\",\n";
    out << "  \"initialCalculationFormula\": \"llround(durationSec * sampleRate) unless exact PCM stream duration or a codec/container policy replaces it\",\n";
    out << "  \"initialRoundingMode\": \"std::llround\",\n";
    out << "  \"lastGranulePosition\": " << result.oggPages.lastGranulePosition << ",\n";
    out << "  \"lastEosGranulePosition\": " << result.oggPages.lastEosGranulePosition << ",\n";
    out << "  \"skipSamplesAtStart\": " << result.packetScan.skipSamplesAtStart << ",\n";
    out << "  \"discardPaddingAtEnd\": " << result.packetScan.discardPaddingAtEnd << ",\n";
    out << "  \"codecDelay\": " << result.fullMetadata.codecDelay << ",\n";
    out << "  \"initialPadding\": " << result.fullMetadata.initialPadding << ",\n";
    out << "  \"trailingPadding\": " << result.fullMetadata.trailingPadding << ",\n";
    out << "  \"decoderRawSampleSum\": " << result.decode.rawDecoderSampleSum << ",\n";
    out << "  \"decoderEffectiveSampleSum\": " << result.decode.effectiveDecoderSampleSum << ",\n";
    out << "  \"resamplerInputFrames\": " << result.decode.resamplerInputFrames << ",\n";
    out << "  \"resamplerOutputFrames\": " << result.decode.resamplerOutputFrames << ",\n";
    out << "  \"resamplerFlushFrames\": " << result.decode.resamplerFlushFrames << ",\n";
    out << "  \"writerFrames\": " << result.decode.writerFrames << ",\n";
    out << "  \"writerBytes\": " << result.decode.writerBytes << ",\n";
    out << "  \"writerFrameRemainderBytes\": " << result.decode.writerFrameRemainderBytes << ",\n";
    out << "  \"finalExactFrames\": " << result.finalExactFrames << ",\n";
    out << "  \"differenceInitialToFinal\": " << result.differenceInitialToFinal << ",\n";
    out << "  \"differenceSec\": " << std::fixed << std::setprecision(12) << result.differenceSec << ",\n";
    out << "  \"firstMomentExactExtentKnown\": " << jsonString(result.firstMomentExactExtentKnown) << ",\n";
    out << "  \"exactExtentAvailableAtProbe\": " << (result.exactExtentAvailableAtProbe ? "true" : "false") << ",\n";
    out << "  \"exactExtentAvailableBeforeInteractiveLoading\": "
        << (result.exactExtentAvailableBeforeInteractiveLoading ? "true" : "false") << ",\n";
    out << "  \"exactExtentAvailableOnlyAfterDecode\": "
        << (result.exactExtentAvailableOnlyAfterDecode ? "true" : "false") << ",\n";
    out << "  \"rootCause\": " << jsonString(result.rootCause) << ",\n";
    out << "  \"productionFrameCountPolicyReason\": "
        << jsonString(result.productionFastProbe.document.frameCountPolicyReason) << "\n";
    out << "}\n";
}

void classify(AuditResult& result) {
    result.initialPublishedFrames = result.productionFastProbe.document.decodedSampleFrames;
    constexpr std::int64_t kAveVoicePyramidFrameAlignment = 1024;
    if (result.initialPublishedFrames > 0) {
        result.aveVoiceAlignedProvisionalFrames =
            ((result.initialPublishedFrames + kAveVoicePyramidFrameAlignment - 1) /
             kAveVoicePyramidFrameAlignment) *
            kAveVoicePyramidFrameAlignment;
    }
    result.initialExtentKind = result.productionFastProbe.document.decodedSampleFramesKind;
    result.initialExtentTrust = result.productionFastProbe.document.decodedSampleFramesTrust;
    result.initialExtentSource = result.productionFastProbe.document.decodedSampleFramesSource;
    result.initialExtentExact = result.initialExtentKind == "exact" ||
        result.initialExtentTrust == "authoritative";
    result.finalExactFrames = result.decode.writerFrames > 0
        ? result.decode.writerFrames
        : result.decode.effectiveDecoderSampleSum;
    result.differenceInitialToFinal = result.finalExactFrames - result.initialPublishedFrames;
    result.differenceAveVoiceProvisionalToFinal =
        result.finalExactFrames - result.aveVoiceAlignedProvisionalFrames;
    if (result.fullMetadata.sampleRate > 0) {
        result.differenceSec =
            static_cast<double>(result.differenceInitialToFinal) /
            static_cast<double>(result.fullMetadata.sampleRate);
    }

    const bool boundedProbeExact =
        result.boundedMetadata.streamCandidates.nearest == result.finalExactFrames ||
        result.boundedMetadata.formatCandidates.nearest == result.finalExactFrames;
    const bool fullProbeExact =
        result.fullMetadata.streamCandidates.nearest == result.finalExactFrames ||
        result.oggPages.lastGranulePosition == result.finalExactFrames ||
        result.oggPages.lastEosGranulePosition == result.finalExactFrames;
    result.exactExtentAvailableAtProbe = boundedProbeExact || fullProbeExact;
    result.exactExtentAvailableBeforeInteractiveLoading = result.exactExtentAvailableAtProbe;
    result.exactExtentAvailableOnlyAfterDecode = !result.exactExtentAvailableAtProbe;
    result.firstMomentExactExtentKnown =
        boundedProbeExact ? "bounded_probe_stream_duration" :
        fullProbeExact ? "full_container_granule_probe" :
        "decode_completed";

    if (result.initialPublishedFrames == 105840640 && result.finalExactFrames == 105840000) {
        if (result.oggPages.lastGranulePosition == result.finalExactFrames &&
            result.productionFastProbe.document.decodedSampleFramesTrust == "unsafe_estimated") {
            result.rootCause = "progress_contract_confuses_estimate_with_exact";
        } else if (result.fullMetadata.streamDurationTs == result.finalExactFrames) {
            result.rootCause = "exact_extent_available_but_ignored";
        } else {
            result.rootCause = "mixed";
        }
        return;
    }

    if (result.initialPublishedFrames == result.finalExactFrames &&
        result.finalExactFrames == 105840000) {
        if (result.aveVoiceAlignedProvisionalFrames == 105840640 &&
            result.productionFastProbe.document.decodedSampleFramesTrust == "unsafe_estimated") {
            result.rootCause = "exact_extent_available_but_ignored";
        } else {
            result.rootCause = "not_reproduced";
        }
        return;
    }

    if (result.fullMetadata.streamCandidates.nearest != result.finalExactFrames &&
        result.decode.resamplerOutputFrames == result.finalExactFrames &&
        result.decode.rawDecoderSampleSum != result.finalExactFrames) {
        result.rootCause = "resampler_frame_count_difference";
        return;
    }

    result.rootCause = "unknown";
}

AuditResult runAudit(const Args& args) {
    AuditResult result;
    result.mediaPath = args.input;
    std::error_code ec;
    result.fileSizeBytes = std::filesystem::file_size(args.input, ec);

    result.avformatVersion = makeVersionSnapshot(
        LIBAVFORMAT_VERSION_MAJOR,
        LIBAVFORMAT_VERSION_MINOR,
        LIBAVFORMAT_VERSION_MICRO,
        avformat_version(),
        "avformat-61.dll");
    result.avcodecVersion = makeVersionSnapshot(
        LIBAVCODEC_VERSION_MAJOR,
        LIBAVCODEC_VERSION_MINOR,
        LIBAVCODEC_VERSION_MICRO,
        avcodec_version(),
        "avcodec-61.dll");
    result.avutilVersion = makeVersionSnapshot(
        LIBAVUTIL_VERSION_MAJOR,
        LIBAVUTIL_VERSION_MINOR,
        LIBAVUTIL_VERSION_MICRO,
        avutil_version(),
        "avutil-59.dll");
    result.swresampleVersion = makeVersionSnapshot(
        LIBSWRESAMPLE_VERSION_MAJOR,
        LIBSWRESAMPLE_VERSION_MINOR,
        LIBSWRESAMPLE_VERSION_MICRO,
        swresample_version(),
        "swresample-5.dll");

    const std::string inputUtf8 = pathText(args.input);
    result.productionFastProbe = Probe::runFastProbe(inputUtf8);
    result.boundedMetadata = inspectMetadata(inputUtf8, true);
    result.fullMetadata = inspectMetadata(inputUtf8, false);
    const int selectedStream = result.fullMetadata.selectedAudioStreamIndex >= 0
        ? result.fullMetadata.selectedAudioStreamIndex
        : result.boundedMetadata.selectedAudioStreamIndex;
    const int sampleRate = result.fullMetadata.sampleRate > 0
        ? result.fullMetadata.sampleRate
        : result.boundedMetadata.sampleRate;
    result.packetScan = inspectPackets(inputUtf8, selectedStream, sampleRate);
    result.oggPages = inspectOggPages(args.input);
    result.decode = decodeCount(inputUtf8);
    classify(result);
    return result;
}

void writeAuditOutputs(const Args& args, const AuditResult& result) {
    std::string error;
    if (!ensureDirectory(args.outputDir, error)) {
        throw std::runtime_error(error);
    }
    writeSummaryJson(args.outputDir / "ogg_vorbis_extent_summary.json", result);
    writePacketTimingJsonl(args.outputDir / "packet_timing.jsonl", result);
    writeDecodeAccountingJson(args.outputDir / "decode_accounting.json", result);
    writeExtentCandidatesJson(args.outputDir / "extent_candidates.json", result);
}

int runSelfTest() {
    const FrameCandidates exactStream = makeCandidates(105840000, AVRational{1, 44100}, 44100);
    if (exactStream.nearest != 105840000 || exactStream.floor != 105840000 || exactStream.ceil != 105840000) {
        std::cerr << "exact stream-duration candidate self-test failed\n";
        return 2;
    }
    const std::int64_t roundedEstimate =
        llroundFramesFromSeconds(105840640.0 / 44100.0, 44100);
    if (roundedEstimate != 105840640) {
        std::cerr << "duration llround candidate self-test failed\n";
        return 3;
    }
    if (105840000 - roundedEstimate != -640) {
        std::cerr << "extent delta self-test failed\n";
        return 4;
    }
    constexpr std::int64_t kAveVoicePyramidFrameAlignment = 1024;
    const std::int64_t alignedProvisional =
        ((105840000 + kAveVoicePyramidFrameAlignment - 1) /
         kAveVoicePyramidFrameAlignment) *
        kAveVoicePyramidFrameAlignment;
    if (alignedProvisional != 105840640) {
        std::cerr << "consumer provisional alignment self-test failed\n";
        return 5;
    }
    std::cout << "AveMediaBridgePcmExtentAudit self-test passed\n";
    return 0;
}

Args parseArgs(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--self-test") {
            args.selfTest = true;
        } else if (arg == "--input" && i + 1 < argc) {
            args.input = argv[++i];
        } else if (arg == "--output-dir" && i + 1 < argc) {
            args.outputDir = argv[++i];
        } else {
            throw std::runtime_error("unknown or incomplete argument: " + arg);
        }
    }
    if (!args.selfTest && args.input.empty()) {
        throw std::runtime_error("usage: AveMediaBridgePcmExtentAudit --input <media> [--output-dir <dir>]");
    }
    if (args.outputDir.empty()) {
        args.outputDir = std::filesystem::current_path() / "research" / "pcm_extent";
    }
    return args;
}

}  // namespace

int main(int argc, char** argv) {
    av_log_set_level(AV_LOG_QUIET);
    try {
        const Args args = parseArgs(argc, argv);
        if (args.selfTest) {
            return runSelfTest();
        }
        trace("ProbeOpened", "mediaPath=" + jsonString(pathText(args.input)));
        const AuditResult result = runAudit(args);
        writeAuditOutputs(args, result);
        trace(
            "ExtentMismatchClassified",
            "initialPublishedFrames=" + std::to_string(result.initialPublishedFrames) +
                " finalExactFrames=" + std::to_string(result.finalExactFrames) +
                " rootCause=" + result.rootCause);
        std::cout << "Wrote PCM extent audit outputs to " << args.outputDir.string() << "\n";
        std::cout << "initialPublishedFrames=" << result.initialPublishedFrames
                  << " finalExactFrames=" << result.finalExactFrames
                  << " differenceInitialToFinal=" << result.differenceInitialToFinal
                  << " rootCause=" << result.rootCause << "\n";
        return result.decode.ok ? 0 : 2;
    } catch (const std::exception& ex) {
        std::cerr << "ERROR: " << ex.what() << "\n";
        return 1;
    }
}
