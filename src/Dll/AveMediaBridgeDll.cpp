#include "AveMediaBridge/AveMediaBridgeApi.hpp"

#include "AveMediaBridge/AveMediaBridge.hpp"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
}

namespace {

constexpr const wchar_t* kVersionString = L"AveMediaBridge 0.1.0 API v1";
constexpr std::int64_t kFastProbeSizeBytes = 4 * 1024 * 1024;
constexpr std::int64_t kFastProbeAnalyzeDurationUs = 3 * AV_TIME_BASE;
thread_local std::wstring g_lastError;

struct FastProbeResult {
    std::string sourcePath;
    std::string formatName;
    std::string formatLongName;
    std::string containerFormat;
    int streamCount = 0;
    int audioStreamCount = 0;
    bool streamInfoFound = false;
    bool hasAudio = false;
    int bestAudioStreamIndex = -1;
    AveMediaBridge::SelectedAudioStreamInfo selectedAudio;
    std::string channelLayout;
    double durationSec = 0.0;
    std::string durationKind = "unknown";
    std::string durationEstimationMethod = "unknown";
    std::int64_t decodedSampleFrames = 0;
    std::string decodedSampleFramesKind = "unknown";
    std::int64_t estimatedDecodedBytes = 0;
    std::string estimatedDecodedBytesKind = "unknown";
    int probeScore = -1;
    std::vector<AveMediaBridge::StreamSummary> streams;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

std::string wideToUtf8(const wchar_t* value) {
    if (!value || value[0] == L'\0') {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        return {};
    }

    std::string text(static_cast<std::size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, text.data(), required, nullptr, nullptr);
    return text;
}

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), -1, nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (required <= 0) {
        codePage = CP_ACP;
        flags = 0;
        required = MultiByteToWideChar(codePage, flags, value.c_str(), -1, nullptr, 0);
    }
    if (required <= 1) {
        return {};
    }

    std::wstring text(static_cast<std::size_t>(required - 1), L'\0');
    MultiByteToWideChar(codePage, flags, value.c_str(), -1, text.data(), required);
    return text;
}

void clearLastError() {
    g_lastError.clear();
}

void setLastErrorText(const wchar_t* text) {
    g_lastError = text ? text : L"";
}

void setLastErrorText(const std::wstring& text) {
    g_lastError = text;
}

void setLastErrorText(const std::string& text) {
    g_lastError = utf8ToWide(text);
    if (g_lastError.empty() && !text.empty()) {
        g_lastError = L"unrepresentable error text";
    }
}

int copyWideText(const std::wstring& text, wchar_t* outBuffer, int outBufferChars) {
    if (!outBuffer || outBufferChars <= 0) {
        return 1;
    }

    const std::size_t capacity = static_cast<std::size_t>(outBufferChars);
    const std::size_t charsToCopy = (std::min)(text.size(), capacity - 1);
    std::copy_n(text.c_str(), charsToCopy, outBuffer);
    outBuffer[charsToCopy] = L'\0';

    return text.size() + 1 <= capacity ? 0 : 2;
}

std::string jsonString(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
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
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    out << '"';
    return out.str();
}

std::string ffErrorString(int err) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buffer, sizeof(buffer));
    return std::string(buffer);
}

std::string rationalToString(AVRational value) {
    std::ostringstream out;
    out << value.num << "/" << value.den;
    return out.str();
}

std::string mediaTypeName(AVMediaType type) {
    const char* name = av_get_media_type_string(type);
    return name ? std::string(name) : std::string("unknown");
}

std::string describeChannelLayout(const AVChannelLayout& layout) {
    if (layout.nb_channels <= 0 || !av_channel_layout_check(&layout)) {
        return {};
    }

    char buffer[128] = {};
    if (av_channel_layout_describe(&layout, buffer, sizeof(buffer)) <= 0) {
        return {};
    }
    return std::string(buffer);
}

bool isPcmCodec(AVCodecID codecId) {
    switch (codecId) {
        case AV_CODEC_ID_PCM_S16LE:
        case AV_CODEC_ID_PCM_S16BE:
        case AV_CODEC_ID_PCM_U16LE:
        case AV_CODEC_ID_PCM_U16BE:
        case AV_CODEC_ID_PCM_S8:
        case AV_CODEC_ID_PCM_U8:
        case AV_CODEC_ID_PCM_MULAW:
        case AV_CODEC_ID_PCM_ALAW:
        case AV_CODEC_ID_PCM_S32LE:
        case AV_CODEC_ID_PCM_S32BE:
        case AV_CODEC_ID_PCM_U32LE:
        case AV_CODEC_ID_PCM_U32BE:
        case AV_CODEC_ID_PCM_S24LE:
        case AV_CODEC_ID_PCM_S24BE:
        case AV_CODEC_ID_PCM_U24LE:
        case AV_CODEC_ID_PCM_U24BE:
        case AV_CODEC_ID_PCM_S24DAUD:
        case AV_CODEC_ID_PCM_ZORK:
        case AV_CODEC_ID_PCM_S16LE_PLANAR:
        case AV_CODEC_ID_PCM_DVD:
        case AV_CODEC_ID_PCM_F32BE:
        case AV_CODEC_ID_PCM_F32LE:
        case AV_CODEC_ID_PCM_F64BE:
        case AV_CODEC_ID_PCM_F64LE:
        case AV_CODEC_ID_PCM_BLURAY:
        case AV_CODEC_ID_PCM_LXF:
        case AV_CODEC_ID_PCM_S8_PLANAR:
        case AV_CODEC_ID_PCM_S24LE_PLANAR:
        case AV_CODEC_ID_PCM_S32LE_PLANAR:
        case AV_CODEC_ID_PCM_S16BE_PLANAR:
        case AV_CODEC_ID_PCM_S64LE:
        case AV_CODEC_ID_PCM_S64BE:
        case AV_CODEC_ID_PCM_F16LE:
        case AV_CODEC_ID_PCM_F24LE:
        case AV_CODEC_ID_PCM_VIDC:
        case AV_CODEC_ID_PCM_SGA:
            return true;
        default:
            return false;
    }
}

AveMediaBridge::StreamSummary makeFastStreamSummary(int index, const AVStream* stream) {
    AveMediaBridge::StreamSummary summary;
    summary.index = index;
    if (!stream || !stream->codecpar) {
        return summary;
    }

    const AVCodecParameters* codecpar = stream->codecpar;
    summary.mediaType = mediaTypeName(codecpar->codec_type);
    summary.codecName = avcodec_get_name(codecpar->codec_id);
    summary.codecId = static_cast<int>(codecpar->codec_id);
    summary.sampleRate = codecpar->sample_rate;
    summary.channels = codecpar->ch_layout.nb_channels;
    summary.bitRate = codecpar->bit_rate;
    summary.timeBase = rationalToString(stream->time_base);
    return summary;
}

int findFirstAudioStream(const AVFormatContext* formatContext) {
    if (!formatContext) {
        return -1;
    }

    for (unsigned int i = 0; i < formatContext->nb_streams; ++i) {
        const AVStream* stream = formatContext->streams[i];
        if (stream && stream->codecpar && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

double secondsFromStreamDuration(const AVStream* stream) {
    if (!stream || stream->duration == AV_NOPTS_VALUE || stream->duration <= 0 ||
        stream->time_base.num <= 0 || stream->time_base.den <= 0) {
        return 0.0;
    }

    return static_cast<double>(stream->duration) * av_q2d(stream->time_base);
}

std::int64_t exactPcmFramesFromStreamDuration(const AVStream* stream, const AVCodecParameters* codecpar) {
    if (!stream || !codecpar || !isPcmCodec(codecpar->codec_id) || codecpar->sample_rate <= 0 ||
        stream->duration == AV_NOPTS_VALUE || stream->duration <= 0 ||
        stream->time_base.num <= 0 || stream->time_base.den <= 0) {
        return 0;
    }

    if (stream->time_base.num == 1 && stream->time_base.den == codecpar->sample_rate) {
        return stream->duration;
    }

    const long double frames =
        static_cast<long double>(stream->duration) *
        static_cast<long double>(stream->time_base.num) *
        static_cast<long double>(codecpar->sample_rate) /
        static_cast<long double>(stream->time_base.den);
    const auto rounded = static_cast<std::int64_t>(std::llround(frames));
    if (rounded > 0 && std::fabs(frames - static_cast<long double>(rounded)) < 0.000001L) {
        return rounded;
    }
    return 0;
}

void fillFastSourceInfo(FastProbeResult& result, const AVFormatContext* formatContext) {
    if (!formatContext) {
        return;
    }

    result.streamCount = static_cast<int>(formatContext->nb_streams);
    result.probeScore = formatContext->probe_score;
    if (formatContext->iformat) {
        result.formatName = formatContext->iformat->name ? formatContext->iformat->name : "";
        result.formatLongName = formatContext->iformat->long_name ? formatContext->iformat->long_name : "";
        result.containerFormat = !result.formatLongName.empty() ? result.formatLongName : result.formatName;
    }
}

void fillFastStreamDetails(FastProbeResult& result, const AVFormatContext* formatContext) {
    if (!formatContext) {
        return;
    }

    result.streams.reserve(formatContext->nb_streams);
    for (unsigned int i = 0; i < formatContext->nb_streams; ++i) {
        const AVStream* stream = formatContext->streams[i];
        if (stream && stream->codecpar && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ++result.audioStreamCount;
        }
        result.streams.push_back(makeFastStreamSummary(static_cast<int>(i), stream));
    }
}

void fillFastSelectedAudio(
    FastProbeResult& result,
    const AVStream* stream,
    const AVCodec* decoder) {
    if (!stream || !stream->codecpar) {
        return;
    }

    const AVCodecParameters* codecpar = stream->codecpar;
    result.hasAudio = true;
    result.selectedAudio.index = static_cast<int>(stream->index);
    result.selectedAudio.codecName = avcodec_get_name(codecpar->codec_id);
    result.selectedAudio.codecId = static_cast<int>(codecpar->codec_id);
    result.selectedAudio.decoderName = decoder && decoder->name ? decoder->name : "";
    result.selectedAudio.sampleRate = codecpar->sample_rate;
    result.selectedAudio.channels = codecpar->ch_layout.nb_channels;
    result.selectedAudio.bitRate = codecpar->bit_rate;
    result.selectedAudio.timeBase = rationalToString(stream->time_base);
    result.channelLayout = describeChannelLayout(codecpar->ch_layout);
}

void estimateFastDurationAndFrames(
    FastProbeResult& result,
    const AVFormatContext* formatContext,
    const AVStream* audioStream) {
    const AVCodecParameters* codecpar = audioStream && audioStream->codecpar ? audioStream->codecpar : nullptr;
    const double streamSeconds = secondsFromStreamDuration(audioStream);
    const std::int64_t exactPcmFrames = exactPcmFramesFromStreamDuration(audioStream, codecpar);

    if (streamSeconds > 0.0) {
        result.durationSec = streamSeconds;
        result.durationEstimationMethod = "from_stream";
        result.durationKind = exactPcmFrames > 0 ? "exact" : "estimated";
    } else if (formatContext && formatContext->duration != AV_NOPTS_VALUE && formatContext->duration > 0) {
        result.durationSec = static_cast<double>(formatContext->duration) / static_cast<double>(AV_TIME_BASE);
        result.durationEstimationMethod = "from_pts";
        result.durationKind = "estimated";
    } else if (formatContext) {
        const std::int64_t bitRate =
            codecpar && codecpar->bit_rate > 0 ? codecpar->bit_rate : formatContext->bit_rate;
        const std::int64_t byteSize = formatContext->pb ? avio_size(formatContext->pb) : -1;
        if (bitRate > 0 && byteSize > 0) {
            result.durationSec = (static_cast<double>(byteSize) * 8.0) / static_cast<double>(bitRate);
            result.durationEstimationMethod = "from_bitrate";
            result.durationKind = "estimated";
        }
    }

    const int sampleRate = codecpar ? codecpar->sample_rate : 0;
    const int channels = codecpar ? codecpar->ch_layout.nb_channels : 0;
    if (exactPcmFrames > 0) {
        result.decodedSampleFrames = exactPcmFrames;
        result.decodedSampleFramesKind = "exact";
    } else if (result.durationSec > 0.0 && sampleRate > 0 && std::isfinite(result.durationSec)) {
        result.decodedSampleFrames =
            static_cast<std::int64_t>(std::llround(result.durationSec * static_cast<double>(sampleRate)));
        result.decodedSampleFramesKind = result.decodedSampleFrames > 0 ? "estimated" : "unknown";
    }

    if (result.decodedSampleFrames > 0 && channels > 0) {
        result.estimatedDecodedBytes =
            result.decodedSampleFrames *
            static_cast<std::int64_t>(channels) *
            static_cast<std::int64_t>(sizeof(float));
        result.estimatedDecodedBytesKind = result.decodedSampleFramesKind;
    }
}

FastProbeResult runFastProbe(const std::string& path) {
    FastProbeResult result;
    result.sourcePath = path;

    AVFormatContext* formatContext = avformat_alloc_context();
    if (!formatContext) {
        result.errors.push_back("avformat_alloc_context failed");
        return result;
    }

    formatContext->probesize = kFastProbeSizeBytes;
    formatContext->max_analyze_duration = kFastProbeAnalyzeDurationUs;

    AVDictionary* options = nullptr;
    av_dict_set(&options, "probesize", "4194304", 0);
    av_dict_set(&options, "analyzeduration", "3000000", 0);

    int ret = avformat_open_input(&formatContext, path.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (ret < 0) {
        result.errors.push_back("avformat_open_input failed: " + ffErrorString(ret));
        if (formatContext) {
            avformat_close_input(&formatContext);
        }
        return result;
    }

    ret = avformat_find_stream_info(formatContext, nullptr);
    fillFastSourceInfo(result, formatContext);
    if (ret < 0) {
        result.errors.push_back("avformat_find_stream_info failed: " + ffErrorString(ret));
        avformat_close_input(&formatContext);
        return result;
    }

    result.streamInfoFound = true;
    fillFastStreamDetails(result, formatContext);

    const AVCodec* decoder = nullptr;
    int audioStreamIndex = av_find_best_stream(formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (audioStreamIndex < 0) {
        const int fallbackAudioStreamIndex = findFirstAudioStream(formatContext);
        if (fallbackAudioStreamIndex >= 0) {
            result.warnings.push_back("av_find_best_stream(audio) failed, using first audio stream: " + ffErrorString(audioStreamIndex));
            audioStreamIndex = fallbackAudioStreamIndex;
        } else {
            result.errors.push_back("no audio stream found: " + ffErrorString(audioStreamIndex));
            avformat_close_input(&formatContext);
            return result;
        }
    }

    result.bestAudioStreamIndex = audioStreamIndex;
    AVStream* audioStream = formatContext->streams[audioStreamIndex];
    if (!decoder && audioStream && audioStream->codecpar) {
        decoder = avcodec_find_decoder(audioStream->codecpar->codec_id);
    }
    fillFastSelectedAudio(result, audioStream, decoder);
    estimateFastDurationAndFrames(result, formatContext, audioStream);

    avformat_close_input(&formatContext);
    return result;
}

double durationSeconds(const AveMediaBridge::AudioImportResult& result) {
    if (result.source.durationSeconds > 0.0) {
        return result.source.durationSeconds;
    }
    return result.audio.durationSeconds();
}

bool hasAudio(const AveMediaBridge::AudioImportResult& result) {
    return result.hasUsableAudio() || result.selectedAudio.index >= 0 || result.probe.audioStreamCount > 0;
}

void writeStringArray(std::ostream& out, const std::vector<std::string>& values) {
    out << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << jsonString(values[i]);
    }
    out << "]";
}

void writeErrorArray(std::ostream& out, const std::string& error) {
    if (error.empty()) {
        out << "[]";
        return;
    }
    out << "[" << jsonString(error) << "]";
}

void writeSelectedAudioJson(std::ostream& out, const AveMediaBridge::SelectedAudioStreamInfo& stream, const char* indent) {
    out << indent << "{\n";
    out << indent << "  \"index\": " << stream.index << ",\n";
    out << indent << "  \"codecName\": " << jsonString(stream.codecName) << ",\n";
    out << indent << "  \"codecId\": " << stream.codecId << ",\n";
    out << indent << "  \"decoderName\": " << jsonString(stream.decoderName) << ",\n";
    out << indent << "  \"sampleRate\": " << stream.sampleRate << ",\n";
    out << indent << "  \"channels\": " << stream.channels << ",\n";
    out << indent << "  \"bitRate\": " << stream.bitRate << ",\n";
    out << indent << "  \"timeBase\": " << jsonString(stream.timeBase) << ",\n";
    out << indent << "  \"decoderSampleFormat\": " << jsonString(stream.decoderSampleFormat) << "\n";
    out << indent << "}";
}

void writeDecodeReportJson(std::ostream& out, const AveMediaBridge::DecodeReport& report, const char* indent) {
    out << indent << "{\n";
    out << indent << "  \"packetsRead\": " << report.packetsRead << ",\n";
    out << indent << "  \"audioPackets\": " << report.audioPackets << ",\n";
    out << indent << "  \"invalidPacketsSkipped\": " << report.invalidPacketsSkipped << ",\n";
    out << indent << "  \"decodedFrames\": " << report.decodedFrames << ",\n";
    out << indent << "  \"outputSamplesPerChannel\": " << report.outputSamplesPerChannel << ",\n";
    out << indent << "  \"outputInterleavedFloatSamples\": " << report.outputInterleavedFloatSamples << ",\n";
    out << indent << "  \"outputSampleRate\": " << report.outputSampleRate << ",\n";
    out << indent << "  \"outputChannels\": " << report.outputChannels << ",\n";
    out << indent << "  \"outputSampleFormat\": " << jsonString(report.outputSampleFormat) << ",\n";
    out << indent << "  \"decoderOpened\": " << (report.decoderOpened ? "true" : "false") << ",\n";
    out << indent << "  \"swrInitialized\": " << (report.swrInitialized ? "true" : "false") << ",\n";
    out << indent << "  \"warningsCount\": " << report.warningsCount << ",\n";
    out << indent << "  \"warnings\": ";
    writeStringArray(out, report.warnings);
    out << "\n";
    out << indent << "}";
}

void writeStreamSummariesJson(std::ostream& out, const std::vector<AveMediaBridge::StreamSummary>& streams, const char* indent) {
    out << indent << "[\n";
    for (std::size_t i = 0; i < streams.size(); ++i) {
        const auto& stream = streams[i];
        out << indent << "  {\n";
        out << indent << "    \"index\": " << stream.index << ",\n";
        out << indent << "    \"mediaType\": " << jsonString(stream.mediaType) << ",\n";
        out << indent << "    \"codecName\": " << jsonString(stream.codecName) << ",\n";
        out << indent << "    \"codecId\": " << stream.codecId << ",\n";
        out << indent << "    \"sampleRate\": " << stream.sampleRate << ",\n";
        out << indent << "    \"channels\": " << stream.channels << ",\n";
        out << indent << "    \"bitRate\": " << stream.bitRate << ",\n";
        out << indent << "    \"timeBase\": " << jsonString(stream.timeBase) << "\n";
        out << indent << "  }" << (i + 1 < streams.size() ? "," : "") << "\n";
    }
    out << indent << "]";
}

bool createParentDirectory(const std::filesystem::path& filePath, std::string& error) {
    const std::filesystem::path parent = filePath.parent_path();
    if (parent.empty()) {
        return true;
    }

    std::error_code fsError;
    std::filesystem::create_directories(parent, fsError);
    if (fsError) {
        error = "failed to create output directory: " + fsError.message();
        return false;
    }
    return true;
}

bool createDirectory(const std::filesystem::path& directory, std::string& error) {
    std::error_code fsError;
    std::filesystem::create_directories(directory, fsError);
    if (fsError) {
        error = "failed to create directory: " + fsError.message();
        return false;
    }
    if (!std::filesystem::is_directory(directory, fsError)) {
        error = "path is not a directory";
        return false;
    }
    if (fsError) {
        error = "failed to inspect directory: " + fsError.message();
        return false;
    }
    return true;
}

bool validateInputPath(const wchar_t* inputPath, std::filesystem::path& inputFsPath, std::string& inputUtf8, std::string& error) {
    if (!inputPath || inputPath[0] == L'\0') {
        error = "inputPath is empty";
        return false;
    }

    inputFsPath = std::filesystem::path(inputPath);
    inputUtf8 = wideToUtf8(inputPath);
    if (inputUtf8.empty()) {
        error = "inputPath could not be converted to UTF-8";
        return false;
    }

    std::error_code fsError;
    if (!std::filesystem::exists(inputFsPath, fsError)) {
        error = fsError ? "failed to inspect inputPath: " + fsError.message() : "inputPath does not exist";
        return false;
    }
    if (!std::filesystem::is_regular_file(inputFsPath, fsError)) {
        error = fsError ? "failed to inspect inputPath: " + fsError.message() : "inputPath is not a regular file";
        return false;
    }
    return true;
}

bool validateOutputPath(const wchar_t* outputPath, std::filesystem::path& outputFsPath, std::string& outputUtf8, std::string& error) {
    if (!outputPath || outputPath[0] == L'\0') {
        error = "output path is empty";
        return false;
    }

    outputFsPath = std::filesystem::path(outputPath);
    outputUtf8 = wideToUtf8(outputPath);
    if (outputUtf8.empty()) {
        error = "output path could not be converted to UTF-8";
        return false;
    }
    return createParentDirectory(outputFsPath, error);
}

bool writeFastProbeJson(
    const std::filesystem::path& outputPath,
    const FastProbeResult& result,
    std::string& error) {
    if (!createParentDirectory(outputPath, error)) {
        return false;
    }

    std::ofstream json(outputPath, std::ios::binary);
    if (!json) {
        error = "failed to open JSON output file";
        return false;
    }

    json << std::fixed << std::setprecision(9);
    json << "{\n";
    json << "  \"apiVersion\": 1,\n";
    json << "  \"schemaVersion\": 2,\n";
    json << "  \"sourcePath\": " << jsonString(result.sourcePath) << ",\n";
    json << "  \"probeMode\": \"fast_v2\",\n";
    json << "  \"hasAudio\": " << (result.hasAudio ? "true" : "false") << ",\n";
    json << "  \"bestAudioStreamIndex\": " << result.bestAudioStreamIndex << ",\n";
    json << "  \"selectedAudioStreamIndex\": " << result.selectedAudio.index << ",\n";
    json << "  \"containerFormat\": " << jsonString(result.containerFormat) << ",\n";
    json << "  \"formatName\": " << jsonString(result.formatName) << ",\n";
    json << "  \"formatLongName\": " << jsonString(result.formatLongName) << ",\n";
    json << "  \"codecName\": " << jsonString(result.selectedAudio.codecName) << ",\n";
    json << "  \"codecId\": " << result.selectedAudio.codecId << ",\n";
    json << "  \"sampleRate\": " << result.selectedAudio.sampleRate << ",\n";
    json << "  \"channels\": " << result.selectedAudio.channels << ",\n";
    json << "  \"channelLayout\": " << jsonString(result.channelLayout) << ",\n";
    json << "  \"frames\": " << result.decodedSampleFrames << ",\n";
    json << "  \"durationSec\": " << result.durationSec << ",\n";
    json << "  \"durationKind\": " << jsonString(result.durationKind) << ",\n";
    json << "  \"durationEstimationMethod\": " << jsonString(result.durationEstimationMethod) << ",\n";
    json << "  \"decodedSampleFrames\": " << result.decodedSampleFrames << ",\n";
    json << "  \"decodedSampleFramesKind\": " << jsonString(result.decodedSampleFramesKind) << ",\n";
    json << "  \"estimatedDecodedBytes\": " << result.estimatedDecodedBytes << ",\n";
    json << "  \"estimatedDecodedBytesKind\": " << jsonString(result.estimatedDecodedBytesKind) << ",\n";
    json << "  \"probeScore\": " << result.probeScore << ",\n";
    json << "  \"streamCount\": " << result.streamCount << ",\n";
    json << "  \"audioStreamCount\": " << result.audioStreamCount << ",\n";
    json << "  \"selectedAudioStream\": ";
    writeSelectedAudioJson(json, result.selectedAudio, "  ");
    json << ",\n";
    json << "  \"streams\": ";
    writeStreamSummariesJson(json, result.streams, "  ");
    json << ",\n";
    json << "  \"warnings\": ";
    writeStringArray(json, result.warnings);
    json << ",\n";
    json << "  \"errors\": ";
    writeStringArray(json, result.errors);
    json << "\n";
    json << "}\n";

    if (!json) {
        error = "failed to write JSON output file";
        return false;
    }
    return true;
}

bool writeAudioInfoJson(
    const std::filesystem::path& outputPath,
    const AveMediaBridge::AudioImportResult& result,
    std::string& error) {
    std::ofstream json(outputPath, std::ios::binary);
    if (!json) {
        error = "failed to open audio_info.json";
        return false;
    }

    json << "{\n";
    json << "  \"sampleRate\": " << result.audio.sampleRate << ",\n";
    json << "  \"channels\": " << result.audio.channels << ",\n";
    json << "  \"frames\": " << result.audio.frameCount() << ",\n";
    json << "  \"sampleFormat\": \"float32\",\n";
    json << "  \"sampleLayout\": \"interleaved\",\n";
    json << "  \"dataFile\": \"original_f32.bin\"\n";
    json << "}\n";

    if (!json) {
        error = "failed to write audio_info.json";
        return false;
    }
    return true;
}

bool writeMetadataJson(
    const std::filesystem::path& outputPath,
    const std::string& sourcePathUtf8,
    const AveMediaBridge::AudioImportResult& result,
    std::string& error) {
    std::ofstream json(outputPath, std::ios::binary);
    if (!json) {
        error = "failed to open metadata.json";
        return false;
    }

    const std::uintmax_t expectedBytes =
        static_cast<std::uintmax_t>(result.audio.frameCount()) *
        static_cast<std::uintmax_t>(result.audio.channels) *
        static_cast<std::uintmax_t>(sizeof(float));

    json << std::fixed << std::setprecision(9);
    json << "{\n";
    json << "  \"apiVersion\": 1,\n";
    json << "  \"sourcePath\": " << jsonString(sourcePathUtf8) << ",\n";
    json << "  \"hasAudio\": " << (hasAudio(result) ? "true" : "false") << ",\n";
    json << "  \"formatName\": " << jsonString(result.source.formatName) << ",\n";
    json << "  \"formatLongName\": " << jsonString(result.source.formatLongName) << ",\n";
    json << "  \"durationSec\": " << durationSeconds(result) << ",\n";
    json << "  \"streamCount\": " << result.source.streamCount << ",\n";
    json << "  \"audioStreamCount\": " << result.probe.audioStreamCount << ",\n";
    json << "  \"selectedAudioStreamIndex\": " << result.selectedAudio.index << ",\n";
    json << "  \"codecName\": " << jsonString(result.selectedAudio.codecName) << ",\n";
    json << "  \"codecId\": " << result.selectedAudio.codecId << ",\n";
    json << "  \"selectedAudioStream\": ";
    writeSelectedAudioJson(json, result.selectedAudio, "  ");
    json << ",\n";
    json << "  \"decodeReport\": ";
    writeDecodeReportJson(json, result.decode, "  ");
    json << ",\n";
    json << "  \"audio\": {\n";
    json << "    \"sampleRate\": " << result.audio.sampleRate << ",\n";
    json << "    \"channels\": " << result.audio.channels << ",\n";
    json << "    \"frames\": " << result.audio.frameCount() << ",\n";
    json << "    \"sampleFormat\": \"float32\",\n";
    json << "    \"sampleLayout\": \"interleaved\",\n";
    json << "    \"dataFile\": \"original_f32.bin\",\n";
    json << "    \"expectedDataBytes\": " << expectedBytes << "\n";
    json << "  },\n";
    json << "  \"audioStats\": {\n";
    json << "    \"rms\": " << result.stats.rms << ",\n";
    json << "    \"peakAbs\": " << result.stats.peakAbs << ",\n";
    json << "    \"minSample\": " << result.stats.minSample << ",\n";
    json << "    \"maxSample\": " << result.stats.maxSample << ",\n";
    json << "    \"clippingRisk\": " << (result.stats.clippingRisk ? "true" : "false") << "\n";
    json << "  },\n";
    json << "  \"streams\": ";
    writeStreamSummariesJson(json, result.probe.streams, "  ");
    json << ",\n";
    json << "  \"warnings\": ";
    writeStringArray(json, result.decode.warnings);
    json << ",\n";
    json << "  \"errors\": ";
    writeErrorArray(json, result.error);
    json << "\n";
    json << "}\n";

    if (!json) {
        error = "failed to write metadata.json";
        return false;
    }
    return true;
}

bool writeF32Binary(
    const std::filesystem::path& outputPath,
    const AveMediaBridge::AudioImportResult& result,
    std::string& error) {
    std::ofstream file(outputPath, std::ios::binary);
    if (!file) {
        error = "failed to open original_f32.bin";
        return false;
    }

    const auto* bytes = reinterpret_cast<const char*>(result.audio.samples.data());
    const std::size_t byteCount = result.audio.samples.size() * sizeof(float);
    if (byteCount > 0) {
        file.write(bytes, static_cast<std::streamsize>(byteCount));
    }

    if (!file) {
        error = "failed to write original_f32.bin";
        return false;
    }
    return true;
}

}  // namespace

int AveMediaBridge_TransformToWav(const wchar_t* inputPath, const wchar_t* outputWavPath, float gain) {
    try {
        std::filesystem::path inputFsPath;
        std::filesystem::path outputFsPath;
        std::string input;
        std::string output;
        std::string error;
        if (!validateInputPath(inputPath, inputFsPath, input, error) ||
            !validateOutputPath(outputWavPath, outputFsPath, output, error)) {
            setLastErrorText(error);
            return 1;
        }

        AveMediaBridge::AveMediaBridge bridge;
        if (!bridge.transformToWav(input, output, gain, error)) {
            setLastErrorText(error.empty() ? "transform failed" : error);
            return 2;
        }

        clearLastError();
        return 0;
    } catch (...) {
        setLastErrorText(L"unexpected exception in AveMediaBridge_TransformToWav");
        return 99;
    }
}

int AveMediaBridge_GetVersionString(wchar_t* outBuffer, int outBufferChars) {
    try {
        const int result = copyWideText(kVersionString, outBuffer, outBufferChars);
        if (result != 0) {
            setLastErrorText(result == 2 ? L"version output buffer is too small" : L"version output buffer is invalid");
        }
        return result;
    } catch (...) {
        setLastErrorText(L"unexpected exception in AveMediaBridge_GetVersionString");
        return 99;
    }
}

int AveMediaBridge_GetLastErrorText(wchar_t* outBuffer, int outBufferChars) {
    try {
        return copyWideText(g_lastError, outBuffer, outBufferChars);
    } catch (...) {
        return 99;
    }
}

int AveMediaBridge_ProbeToJson(const wchar_t* inputPath, const wchar_t* outputJsonPath) {
    try {
        std::filesystem::path inputFsPath;
        std::filesystem::path outputFsPath;
        std::string input;
        std::string outputUtf8;
        std::string error;
        if (!validateInputPath(inputPath, inputFsPath, input, error) ||
            !validateOutputPath(outputJsonPath, outputFsPath, outputUtf8, error)) {
            setLastErrorText(error);
            return 1;
        }

        FastProbeResult probe = runFastProbe(input);
        if (!writeFastProbeJson(outputFsPath, probe, error)) {
            setLastErrorText(error);
            return 3;
        }

        if (!probe.errors.empty() && !probe.streamInfoFound) {
            setLastErrorText(probe.errors.front());
            return 2;
        }

        clearLastError();
        return 0;
    } catch (...) {
        setLastErrorText(L"unexpected exception in AveMediaBridge_ProbeToJson");
        return 99;
    }
}

int AveMediaBridge_ImportAudioToSession(const wchar_t* inputPath, const wchar_t* sessionMediaDir) {
    try {
        std::filesystem::path inputFsPath;
        std::string input;
        std::string error;
        if (!validateInputPath(inputPath, inputFsPath, input, error)) {
            setLastErrorText(error);
            return 1;
        }
        if (!sessionMediaDir || sessionMediaDir[0] == L'\0') {
            setLastErrorText(L"sessionMediaDir is empty");
            return 1;
        }

        const std::filesystem::path sessionDir(sessionMediaDir);
        if (!createDirectory(sessionDir, error)) {
            setLastErrorText(error);
            return 1;
        }

        AveMediaBridge::AveMediaBridge bridge;
        AveMediaBridge::AudioImportResult imported = bridge.importAudio(input);
        if (!imported.hasUsableAudio()) {
            setLastErrorText(imported.error.empty() ? "audio import failed" : imported.error);
            return 2;
        }

        const std::filesystem::path metadataPath = sessionDir / L"metadata.json";
        const std::filesystem::path audioInfoPath = sessionDir / L"audio_info.json";
        const std::filesystem::path audioDataPath = sessionDir / L"original_f32.bin";

        if (!writeF32Binary(audioDataPath, imported, error) ||
            !writeAudioInfoJson(audioInfoPath, imported, error) ||
            !writeMetadataJson(metadataPath, input, imported, error)) {
            setLastErrorText(error);
            return 3;
        }

        clearLastError();
        return 0;
    } catch (...) {
        setLastErrorText(L"unexpected exception in AveMediaBridge_ImportAudioToSession");
        return 99;
    }
}

const wchar_t* AveMediaBridge_GetVersion() {
    return kVersionString;
}
