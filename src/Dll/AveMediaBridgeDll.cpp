#include "AveMediaBridge/AveMediaBridgeApi.hpp"

#include "AveMediaBridge/AveMediaBridge.hpp"
#include "../Diagnostics/FullScaleClipDiagnostics.hpp"
#include "../Ffmpeg/FfmpegDeleters.hpp"
#include "../Ffmpeg/FfmpegStreamSelection.hpp"
#include "../Probe/MediaProbeService.hpp"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace ClipDiag = AveMediaBridge::Diagnostics;
namespace Ffmpeg = AveMediaBridge::Ffmpeg;
namespace Probe = AveMediaBridge::Probe;

constexpr const wchar_t* kVersionString = L"AveMediaBridge 0.1.0 API v1";
constexpr std::int64_t kImportProgressFrameInterval = 16 * 1024;
constexpr std::uint32_t kDraftWaveformFramesPerBin = 128;
constexpr std::uint32_t kDraftWaveformMaxBinsPerChunk = 64;
constexpr const wchar_t* kFullScaleClipDiagEnvVar = L"AVEMEDIABRIDGE_FULL_SCALE_CLIP_DIAG";
constexpr const char* kFullScaleClipDiagLogPrefix = "[AVEMEDIABRIDGE_FULL_SCALE_CLIP_DIAG]";
constexpr const char* kFullScaleClipResultLogPrefix = "[AVEMEDIABRIDGE_FULL_SCALE_CLIP_RESULT]";
thread_local std::wstring g_lastError;

struct StreamingImportResult {
    bool ok = false;
    std::string error;
    AveMediaBridge::SourceMediaInfo source;
    AveMediaBridge::MediaProbeDetails probe;
    AveMediaBridge::SelectedAudioStreamInfo selectedAudio;
    AveMediaBridge::DecodeReport decode;
    AveMediaBridge::AudioStats stats;
    int sampleRate = 0;
    int channels = 0;
    std::int64_t framesWritten = 0;
    std::int64_t interleavedFloatSamplesWritten = 0;
    std::int64_t bytesWritten = 0;
    std::int64_t expectedBytes = 0;
    std::int64_t preflightEstimatedFrames = 0;
    std::int64_t preflightEstimatedBytes = 0;
    std::string preflightEstimateKind = "unknown";
    bool diskPreflightChecked = false;
    bool diskPreflightEstimateKnown = false;
    std::uintmax_t diskPreflightAvailableBytes = 0;
    bool canceled = false;
};

struct StreamingStatsAccumulator {
    long double sumSquares = 0.0L;
    std::int64_t sampleCount = 0;
    float minSample = std::numeric_limits<float>::infinity();
    float maxSample = -std::numeric_limits<float>::infinity();
    float peakAbs = 0.0f;

    void addSamples(const float* samples, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            const float sample = samples[i];
            sumSquares += static_cast<long double>(sample) * static_cast<long double>(sample);
            if (sample < minSample) {
                minSample = sample;
            }
            if (sample > maxSample) {
                maxSample = sample;
            }

            const float absSample = std::fabs(sample);
            if (absSample > peakAbs) {
                peakAbs = absSample;
            }
            ++sampleCount;
        }
    }

    AveMediaBridge::AudioStats finish() const {
        AveMediaBridge::AudioStats result;
        if (sampleCount <= 0) {
            return result;
        }

        result.rms = static_cast<double>(std::sqrt(sumSquares / static_cast<long double>(sampleCount)));
        result.peakAbs = peakAbs;
        result.minSample = minSample;
        result.maxSample = maxSample;
        result.clippingRisk = peakAbs > 1.0f;
        return result;
    }
};

struct StreamingSwrState {
    SwrContext* ctx = nullptr;
    AVChannelLayout layout{};
    AVSampleFormat inputFormat = AV_SAMPLE_FMT_NONE;
    int sampleRate = 0;
    int channels = 0;
    bool initialized = false;
    std::vector<float> outputBuffer;
};

struct DraftWaveformState {
    std::uint64_t nextFrame = 0;
    std::uint64_t pendingChunkFirstFrame = 0;
    std::uint32_t currentBinFrames = 0;
    float currentMin = 0.0f;
    float currentMax = 0.0f;
    double currentSumSquares = 0.0;
    double currentSumAbs = 0.0;
    bool currentBinHasSamples = false;
    std::vector<float> pendingMinMaxPairs;
    std::vector<double> pendingSumSquaresPerBin;
    std::vector<double> pendingSumAbsPerBin;
    std::vector<std::uint64_t> pendingFrameCountPerBin;
};

struct StreamingImportCallbacks {
    AveMediaBridgeProgressCallback onProgress = nullptr;
    AveMediaBridgeCancelCallback shouldCancel = nullptr;
    AveMediaBridgeWaveformChunkCallback onWaveformChunk = nullptr;
    void* userData = nullptr;
    std::int64_t lastProgressFrames = -1;
    bool progressEmitted = false;
    DraftWaveformState waveform;
};

struct FullScaleClipDiagnostic {
    bool enabled = false;
    std::string path;
    std::string codecName;
    std::string decoderName;
    std::string inputSampleFormatName;
    std::string outputSampleFormatName = "flt";
    ClipDiag::FullScaleSampleStats preSwr;
    ClipDiag::FullScaleSampleStats postSwr;
};

bool fullScaleClipDiagEnabledFromEnvironment() {
    wchar_t value[32] = {};
    const DWORD copied = GetEnvironmentVariableW(
        kFullScaleClipDiagEnvVar,
        value,
        static_cast<DWORD>(sizeof(value) / sizeof(value[0])));
    if (copied == 0) {
        return false;
    }
    if (copied == 1 && value[0] == L'0') {
        return false;
    }
    if ((value[0] == L'f' || value[0] == L'F') &&
        (value[1] == L'a' || value[1] == L'A')) {
        return false;
    }
    return true;
}

std::string fullScaleBoolText(bool value) {
    return value ? "yes" : "no";
}

std::string fullScaleDoubleText(double value) {
    if (!std::isfinite(value)) {
        return "nan";
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(9) << value;
    return out.str();
}

std::string fullScalePercentText(std::uint64_t count, std::uint64_t sampleCount) {
    std::ostringstream out;
    const double percent = sampleCount > 0
        ? (static_cast<double>(count) * 100.0) / static_cast<double>(sampleCount)
        : 0.0;
    out << std::fixed << std::setprecision(9) << percent;
    return out.str();
}

std::string fullScalePlanarText(const ClipDiag::FullScaleSampleStats& stats) {
    if (stats.planarMixed) {
        return "mixed";
    }
    return stats.planar ? "yes" : "no";
}

std::uint64_t exactOneCount(const ClipDiag::FullScaleSampleStats& stats) {
    return stats.exactPositiveOneCount + stats.exactNegativeOneCount;
}

bool hasOversAboveOne(const ClipDiag::FullScaleSampleStats& stats) {
    return stats.abovePositiveOneCount > 0 || stats.belowNegativeOneCount > 0;
}

void logFullScaleStageStats(
    const FullScaleClipDiagnostic& diagnostic,
    const char* stage,
    const ClipDiag::FullScaleSampleStats& stats) {
    std::ostringstream out;
    out << kFullScaleClipDiagLogPrefix
        << " path=\"" << diagnostic.path << "\""
        << " codecName=" << diagnostic.codecName
        << " decoderName=" << diagnostic.decoderName
        << " inputSampleFormat=" << diagnostic.inputSampleFormatName
        << " outputSampleFormat=" << diagnostic.outputSampleFormatName
        << " stage=" << stage
        << " sampleFormatName=" << stats.sampleFormatName
        << " planar=" << fullScalePlanarText(stats)
        << " channels=" << stats.channels
        << " frames=" << stats.frames
        << " sampleCount=" << stats.sampleCount
        << " finiteCount=" << stats.finiteCount
        << " nonFiniteCount=" << stats.nonFiniteCount
        << " min=" << fullScaleDoubleText(stats.finiteCount > 0 ? stats.minValue : std::numeric_limits<double>::quiet_NaN())
        << " max=" << fullScaleDoubleText(stats.finiteCount > 0 ? stats.maxValue : std::numeric_limits<double>::quiet_NaN())
        << " maxAbs=" << fullScaleDoubleText(stats.maxAbs)
        << " exactPositiveOne=" << stats.exactPositiveOneCount
        << " exactNegativeOne=" << stats.exactNegativeOneCount
        << " abovePositiveOne=" << stats.abovePositiveOneCount
        << " belowNegativeOne=" << stats.belowNegativeOneCount
        << " nearPositiveOne=" << stats.nearPositiveOneCount
        << " nearNegativeOne=" << stats.nearNegativeOneCount
        << " percentExactPositiveOne=" << fullScalePercentText(stats.exactPositiveOneCount, stats.sampleCount)
        << " percentExactNegativeOne=" << fullScalePercentText(stats.exactNegativeOneCount, stats.sampleCount);
    if (!stats.supported) {
        out << " unsupported=yes unsupportedReason=\"" << stats.unsupportedReason << "\"";
    }
    std::cout << out.str() << "\n";
}

void logFullScaleClipResult(const FullScaleClipDiagnostic& diagnostic) {
    const std::uint64_t preExact = exactOneCount(diagnostic.preSwr);
    const std::uint64_t postExact = exactOneCount(diagnostic.postSwr);
    const bool swrIntroducedExactOne = diagnostic.preSwr.supported && preExact == 0 && postExact > 0;
    const char* exactOneFirstAppearsAt =
        preExact > 0 ? "pre_swr" :
        postExact > 0 ? "post_swr" :
        "not_found";
    const char* likelyCause =
        !diagnostic.preSwr.supported ? "unsupported" :
        preExact > 0 ? "source_or_decoder_full_scale" :
        postExact > 0 ? "swr_float_conversion" :
        "no_full_scale";

    std::cout << kFullScaleClipResultLogPrefix
              << " path=\"" << diagnostic.path << "\""
              << " exactOneFirstAppearsAt=" << exactOneFirstAppearsAt
              << " swrIntroducedExactOne=" << fullScaleBoolText(swrIntroducedExactOne)
              << " rawHadOversAboveOne=" << fullScaleBoolText(hasOversAboveOne(diagnostic.preSwr))
              << " outputHadOversAboveOne=" << fullScaleBoolText(hasOversAboveOne(diagnostic.postSwr))
              << " likelyCause=" << likelyCause
              << "\n";
}

void logFullScaleClipDiagnostic(const FullScaleClipDiagnostic& diagnostic) {
    if (!diagnostic.enabled) {
        return;
    }
    logFullScaleStageStats(diagnostic, "pre_swr", diagnostic.preSwr);
    logFullScaleStageStats(diagnostic, "post_swr", diagnostic.postSwr);
    logFullScaleClipResult(diagnostic);
}

void analyzePreSwrFrame(
    FullScaleClipDiagnostic* diagnostic,
    const AVFrame* frame,
    const AVCodecContext* decoder) {
    if (!diagnostic || !diagnostic->enabled) {
        return;
    }
    const int fallbackChannels = decoder ? decoder->ch_layout.nb_channels : 0;
    ClipDiag::FullScaleSampleStats frameStats;
    ClipDiag::analyzeFrameSamples(frame, fallbackChannels, frameStats);
    if (diagnostic->inputSampleFormatName.empty()) {
        diagnostic->inputSampleFormatName = frameStats.sampleFormatName;
    }
    ClipDiag::mergeSampleStats(diagnostic->preSwr, frameStats);
}

void analyzePostSwrSamples(
    FullScaleClipDiagnostic* diagnostic,
    const float* samples,
    int frames,
    int channels) {
    if (!diagnostic || !diagnostic->enabled) {
        return;
    }
    ClipDiag::FullScaleSampleStats chunkStats;
    ClipDiag::analyzeInterleavedFloatSamples(samples, frames, channels, chunkStats);
    ClipDiag::mergeSampleStats(diagnostic->postSwr, chunkStats);
}

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

void addStreamingWarning(StreamingImportResult& result, const std::string& warning) {
    ++result.decode.warningsCount;
    if (result.decode.warnings.size() < 16) {
        result.decode.warnings.push_back(warning);
    }
}

void fillStreamingSourceInfo(StreamingImportResult& result, const AVFormatContext* formatContext, const std::string& path) {
    result.source.inputPath = path;
    if (!formatContext) {
        return;
    }

    result.source.streamCount = static_cast<int>(formatContext->nb_streams);
    if (formatContext->duration != AV_NOPTS_VALUE && formatContext->duration > 0) {
        result.source.durationSeconds = static_cast<double>(formatContext->duration) / static_cast<double>(AV_TIME_BASE);
    }

    if (formatContext->iformat) {
        result.source.formatName = formatContext->iformat->name ? formatContext->iformat->name : "";
        result.source.formatLongName = formatContext->iformat->long_name ? formatContext->iformat->long_name : "";
    }
}

void fillStreamingProbeDetails(StreamingImportResult& result, const AVFormatContext* formatContext) {
    if (!formatContext) {
        return;
    }

    result.probe.streamInfoFound = true;
    result.probe.streams.reserve(formatContext->nb_streams);
    for (unsigned int i = 0; i < formatContext->nb_streams; ++i) {
        const AVStream* stream = formatContext->streams[i];
        if (stream && stream->codecpar && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ++result.probe.audioStreamCount;
        }
        result.probe.streams.push_back(Probe::makeStreamSummary(static_cast<int>(i), stream));
    }
}

void fillStreamingSelectedAudioInfo(
    StreamingImportResult& result,
    const AVStream* stream,
    const AVCodec* decoder,
    const AVCodecContext* decoderContext) {
    if (!stream || !stream->codecpar) {
        return;
    }

    const AVCodecParameters* codecpar = stream->codecpar;
    result.selectedAudio.index = static_cast<int>(stream->index);
    result.selectedAudio.codecName = avcodec_get_name(codecpar->codec_id);
    result.selectedAudio.codecId = static_cast<int>(codecpar->codec_id);
    result.selectedAudio.decoderName = decoder && decoder->name ? decoder->name : "";
    result.selectedAudio.sampleRate = decoderContext ? decoderContext->sample_rate : codecpar->sample_rate;
    result.selectedAudio.channels = decoderContext ? decoderContext->ch_layout.nb_channels : codecpar->ch_layout.nb_channels;
    result.selectedAudio.bitRate = codecpar->bit_rate;
    result.selectedAudio.timeBase = Probe::rationalToString(stream->time_base);
}

bool estimateDecodedBytesForPreflight(
    const AVFormatContext* formatContext,
    const AVStream* audioStream,
    const std::string& path,
    std::int64_t& estimatedFrames,
    std::int64_t& estimatedBytes,
    std::string& estimateKind) {
    return Probe::estimateDecodedBytesForPreflight(
        formatContext,
        audioStream,
        path,
        estimatedFrames,
        estimatedBytes,
        estimateKind);
}

std::uint64_t nonNegativeToUint64(std::int64_t value) {
    return value > 0 ? static_cast<std::uint64_t>(value) : 0;
}

double progressRatio01(const StreamingImportResult& result) {
    if (result.preflightEstimatedFrames > 0) {
        const double ratio =
            static_cast<double>(result.framesWritten) /
            static_cast<double>(result.preflightEstimatedFrames);
        return (std::min)(1.0, (std::max)(0.0, ratio));
    }
    if (result.preflightEstimatedBytes > 0) {
        const double ratio =
            static_cast<double>(result.bytesWritten) /
            static_cast<double>(result.preflightEstimatedBytes);
        return (std::min)(1.0, (std::max)(0.0, ratio));
    }
    return -1.0;
}

float sanitizeWaveformSample(float sample) {
    return std::isfinite(sample) ? sample : 0.0f;
}

float downmixFrameToMono(const float* frameSamples, int channels) {
    double sum = 0.0;
    for (int channel = 0; channel < channels; ++channel) {
        sum += static_cast<double>(sanitizeWaveformSample(frameSamples[channel]));
    }
    return static_cast<float>(sum / static_cast<double>(channels));
}

bool emitPendingDraftWaveformChunk(
    StreamingImportCallbacks* callbacks,
    int sampleRate,
    int channels,
    std::string& error) {
    if (!callbacks || !callbacks->onWaveformChunk || callbacks->waveform.pendingMinMaxPairs.empty()) {
        return true;
    }

    const std::size_t pairCount = callbacks->waveform.pendingMinMaxPairs.size() / 2;
    if (pairCount == 0 || pairCount > (std::numeric_limits<std::uint32_t>::max)()) {
        error = "draft waveform bin count overflow";
        return false;
    }
    if (callbacks->waveform.pendingSumSquaresPerBin.size() != pairCount ||
        callbacks->waveform.pendingSumAbsPerBin.size() != pairCount ||
        callbacks->waveform.pendingFrameCountPerBin.size() != pairCount) {
        error = "draft waveform energy bin count mismatch";
        return false;
    }

    AveMediaBridgeWaveformChunk chunk{};
    chunk.structSize = sizeof(chunk);
    chunk.firstFrame = callbacks->waveform.pendingChunkFirstFrame;
    chunk.framesPerBin = kDraftWaveformFramesPerBin;
    chunk.binCount = static_cast<std::uint32_t>(pairCount);
    chunk.valuesPerBin = 2;
    chunk.sampleRate = sampleRate;
    chunk.channels = channels;
    chunk.minMaxPairs = callbacks->waveform.pendingMinMaxPairs.data();
    chunk.flags = AVEMEDIABRIDGE_WAVEFORM_CHUNK_FLAG_LONG_FORM_ENERGY;
    chunk.sumSquaresPerBin = callbacks->waveform.pendingSumSquaresPerBin.data();
    chunk.sumAbsPerBin = callbacks->waveform.pendingSumAbsPerBin.data();
    chunk.frameCountPerBin = callbacks->waveform.pendingFrameCountPerBin.data();

    try {
        callbacks->onWaveformChunk(&chunk, callbacks->userData);
    } catch (...) {
        error = "waveform chunk callback threw an exception";
        return false;
    }

    callbacks->waveform.pendingMinMaxPairs.clear();
    callbacks->waveform.pendingSumSquaresPerBin.clear();
    callbacks->waveform.pendingSumAbsPerBin.clear();
    callbacks->waveform.pendingFrameCountPerBin.clear();
    return true;
}

void finishCurrentDraftWaveformBin(DraftWaveformState& waveform) {
    if (!waveform.currentBinHasSamples) {
        return;
    }

    if (waveform.pendingMinMaxPairs.empty()) {
        waveform.pendingChunkFirstFrame =
            waveform.nextFrame - static_cast<std::uint64_t>(waveform.currentBinFrames);
    }
    waveform.pendingMinMaxPairs.push_back(waveform.currentMin);
    waveform.pendingMinMaxPairs.push_back(waveform.currentMax);
    waveform.pendingSumSquaresPerBin.push_back(waveform.currentSumSquares);
    waveform.pendingSumAbsPerBin.push_back(waveform.currentSumAbs);
    waveform.pendingFrameCountPerBin.push_back(waveform.currentBinFrames);
    waveform.currentBinFrames = 0;
    waveform.currentMin = 0.0f;
    waveform.currentMax = 0.0f;
    waveform.currentSumSquares = 0.0;
    waveform.currentSumAbs = 0.0;
    waveform.currentBinHasSamples = false;
}

bool emitDraftWaveformSamples(
    StreamingImportCallbacks* callbacks,
    const float* interleavedSamples,
    int frameCount,
    int channels,
    int sampleRate,
    std::uint64_t firstFrame,
    std::string& error) {
    if (!callbacks || !callbacks->onWaveformChunk || frameCount <= 0) {
        return true;
    }
    if (!interleavedSamples || channels <= 0 || sampleRate <= 0) {
        error = "draft waveform callback received invalid audio shape";
        return false;
    }
    if (callbacks->waveform.nextFrame != firstFrame) {
        error = "draft waveform frame counter mismatch";
        return false;
    }

    DraftWaveformState& waveform = callbacks->waveform;
    for (int frame = 0; frame < frameCount; ++frame) {
        const float mono = downmixFrameToMono(
            interleavedSamples + static_cast<std::size_t>(frame) * static_cast<std::size_t>(channels),
            channels);

        if (!waveform.currentBinHasSamples) {
            waveform.currentMin = mono;
            waveform.currentMax = mono;
            waveform.currentBinHasSamples = true;
        } else {
            waveform.currentMin = (std::min)(waveform.currentMin, mono);
            waveform.currentMax = (std::max)(waveform.currentMax, mono);
        }

        const double monoDouble = static_cast<double>(mono);
        waveform.currentSumSquares += monoDouble * monoDouble;
        waveform.currentSumAbs += std::abs(monoDouble);
        ++waveform.currentBinFrames;
        ++waveform.nextFrame;
        if (waveform.currentBinFrames == kDraftWaveformFramesPerBin) {
            finishCurrentDraftWaveformBin(waveform);
            if (waveform.pendingMinMaxPairs.size() / 2 >= kDraftWaveformMaxBinsPerChunk &&
                !emitPendingDraftWaveformChunk(callbacks, sampleRate, channels, error)) {
                return false;
            }
        }
    }

    return true;
}

bool flushDraftWaveform(
    StreamingImportCallbacks* callbacks,
    const StreamingImportResult& result,
    std::string& error) {
    if (!callbacks || !callbacks->onWaveformChunk) {
        return true;
    }

    finishCurrentDraftWaveformBin(callbacks->waveform);
    return emitPendingDraftWaveformChunk(callbacks, result.sampleRate, result.channels, error);
}

bool emitImportProgress(
    StreamingImportCallbacks* callbacks,
    const StreamingImportResult& result,
    bool forceFinal,
    std::string& error) {
    if (!callbacks || !callbacks->onProgress) {
        return true;
    }
    if (!forceFinal &&
        callbacks->lastProgressFrames >= 0 &&
        result.framesWritten - callbacks->lastProgressFrames < kImportProgressFrameInterval) {
        return true;
    }
    if (callbacks->lastProgressFrames > result.framesWritten) {
        return true;
    }
    AveMediaBridgeImportProgress progress{};
    progress.structSize = sizeof(progress);
    progress.framesWritten = nonNegativeToUint64(result.framesWritten);
    progress.bytesWritten = nonNegativeToUint64(result.bytesWritten);
    progress.estimatedTotalFrames = nonNegativeToUint64(result.preflightEstimatedFrames);
    progress.estimatedTotalBytes = nonNegativeToUint64(result.preflightEstimatedBytes);
    progress.availableEndSec =
        result.sampleRate > 0
            ? static_cast<double>(result.framesWritten) / static_cast<double>(result.sampleRate)
            : 0.0;
    progress.progress01 =
        forceFinal && (result.preflightEstimatedFrames > 0 || result.preflightEstimatedBytes > 0)
            ? 1.0
            : progressRatio01(result);
    progress.sampleRate = result.sampleRate;
    progress.channels = result.channels;
    progress.flags = forceFinal ? 1u : 0u;

    try {
        callbacks->onProgress(&progress, callbacks->userData);
    } catch (...) {
        error = "progress callback threw an exception";
        return false;
    }

    callbacks->lastProgressFrames = result.framesWritten;
    callbacks->progressEmitted = true;
    return true;
}

bool pollImportCancel(
    StreamingImportCallbacks* callbacks,
    StreamingImportResult& result) {
    if (!callbacks || !callbacks->shouldCancel) {
        return true;
    }

    int cancel = 0;
    try {
        cancel = callbacks->shouldCancel(callbacks->userData);
    } catch (...) {
        result.error = "cancel callback threw an exception";
        return false;
    }

    if (cancel != 0) {
        result.canceled = true;
        result.error = "streaming session import canceled";
        return false;
    }
    return true;
}

bool checkDiskPreflight(
    const std::filesystem::path& sessionDir,
    StreamingImportResult& result,
    std::string& error) {
    result.diskPreflightChecked = true;
    if (result.preflightEstimatedBytes <= 0) {
        addStreamingWarning(result, "disk preflight decoded byte estimate unknown; continuing without size gate");
        return true;
    }

    std::error_code fsError;
    const std::filesystem::space_info space = std::filesystem::space(sessionDir, fsError);
    if (fsError) {
        addStreamingWarning(result, "disk preflight free-space check failed: " + fsError.message());
        return true;
    }

    result.diskPreflightAvailableBytes = space.available;
    constexpr std::uintmax_t kDiskPreflightHeadroomBytes = 1024 * 1024;
    const std::uintmax_t requiredBytes =
        static_cast<std::uintmax_t>(result.preflightEstimatedBytes) + kDiskPreflightHeadroomBytes;
    if (space.available < requiredBytes) {
        std::ostringstream out;
        out << "not enough free disk space for session import: estimated "
            << result.preflightEstimatedBytes
            << " decoded bytes, available "
            << space.available
            << " bytes";
        error = out.str();
        return false;
    }
    return true;
}

int copyStreamingFrameLayout(AVChannelLayout* dst, const AVFrame* frame, const AVCodecContext* decoder) {
    if (frame->ch_layout.nb_channels > 0 && av_channel_layout_check(&frame->ch_layout)) {
        return av_channel_layout_copy(dst, &frame->ch_layout);
    }

    if (decoder->ch_layout.nb_channels > 0 && av_channel_layout_check(&decoder->ch_layout)) {
        return av_channel_layout_copy(dst, &decoder->ch_layout);
    }

    const int channels = (std::max)(frame->ch_layout.nb_channels, decoder->ch_layout.nb_channels);
    if (channels <= 0) {
        return AVERROR(EINVAL);
    }

    av_channel_layout_default(dst, channels);
    return 0;
}

int ensureStreamingSwr(
    StreamingSwrState& swr,
    const AVFrame* frame,
    const AVCodecContext* decoder,
    StreamingImportResult& result) {
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
    int ret = copyStreamingFrameLayout(&inputLayout, frame, decoder);
    if (ret < 0) {
        result.error = "unable to determine decoded channel layout: " + ffErrorString(ret);
        return ret;
    }

    ret = av_channel_layout_copy(&swr.layout, &inputLayout);
    if (ret < 0) {
        av_channel_layout_uninit(&inputLayout);
        result.error = "unable to copy output channel layout: " + ffErrorString(ret);
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
        result.error = "swr_alloc_set_opts2 failed: " + ffErrorString(ret);
        return ret;
    }

    ret = swr_init(swr.ctx);
    if (ret < 0) {
        result.error = "swr_init failed: " + ffErrorString(ret);
        return ret;
    }

    swr.initialized = true;
    result.decode.swrInitialized = true;
    result.decode.outputSampleRate = swr.sampleRate;
    result.decode.outputChannels = swr.channels;
    result.sampleRate = swr.sampleRate;
    result.channels = swr.channels;

    const char* sampleFormatName = av_get_sample_fmt_name(inputFormat);
    result.selectedAudio.decoderSampleFormat = sampleFormatName ? sampleFormatName : "";
    return 0;
}

bool addWrittenFrames(StreamingImportResult& result, int frameCount, std::string& error) {
    if (frameCount <= 0) {
        return true;
    }
    if (result.channels <= 0) {
        error = "streaming import output channel count is invalid";
        return false;
    }

    const auto frames = static_cast<std::int64_t>(frameCount);
    if (result.framesWritten > (std::numeric_limits<std::int64_t>::max)() - frames) {
        error = "streaming import frame count overflow";
        return false;
    }

    const auto samples = frames * static_cast<std::int64_t>(result.channels);
    if (result.interleavedFloatSamplesWritten > (std::numeric_limits<std::int64_t>::max)() - samples) {
        error = "streaming import sample count overflow";
        return false;
    }

    const auto bytes = samples * static_cast<std::int64_t>(sizeof(float));
    if (result.bytesWritten > (std::numeric_limits<std::int64_t>::max)() - bytes) {
        error = "streaming import byte count overflow";
        return false;
    }

    result.framesWritten += frames;
    result.interleavedFloatSamplesWritten += samples;
    result.bytesWritten += bytes;
    result.decode.outputSamplesPerChannel += frames;
    result.decode.outputInterleavedFloatSamples += samples;
    return true;
}

int writeConvertedStreamingSamples(
    StreamingSwrState& swr,
    const AVFrame* frame,
    const AVCodecContext* decoder,
    std::ofstream& output,
    StreamingStatsAccumulator& stats,
    StreamingImportResult& result,
    StreamingImportCallbacks* callbacks,
    FullScaleClipDiagnostic* diagnostic) {
    int ret = ensureStreamingSwr(swr, frame, decoder, result);
    if (ret < 0) {
        return ret;
    }
    if (diagnostic && diagnostic->enabled && diagnostic->inputSampleFormatName.empty()) {
        diagnostic->inputSampleFormatName = ClipDiag::sampleFormatName(swr.inputFormat);
    }
    analyzePreSwrFrame(diagnostic, frame, decoder);

    const int outCapacity = swr_get_out_samples(swr.ctx, frame->nb_samples);
    if (outCapacity < 0) {
        result.error = "swr_get_out_samples failed: " + ffErrorString(outCapacity);
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
        result.error = "swr_convert failed: " + ffErrorString(ret);
        return ret;
    }
    if (ret == 0) {
        return 0;
    }

    const std::size_t writtenSamples = static_cast<std::size_t>(ret) * static_cast<std::size_t>(swr.channels);
    analyzePostSwrSamples(diagnostic, swr.outputBuffer.data(), ret, swr.channels);
    const std::size_t writtenBytes = writtenSamples * sizeof(float);
    output.write(reinterpret_cast<const char*>(swr.outputBuffer.data()), static_cast<std::streamsize>(writtenBytes));
    output.flush();
    if (!output) {
        result.error = "failed to write original_f32.bin";
        return AVERROR(EIO);
    }

    stats.addSamples(swr.outputBuffer.data(), writtenSamples);
    const std::uint64_t firstOutputFrame = nonNegativeToUint64(result.framesWritten);
    std::string countError;
    if (!addWrittenFrames(result, ret, countError)) {
        result.error = countError;
        return AVERROR(EOVERFLOW);
    }
    if (!emitDraftWaveformSamples(
            callbacks,
            swr.outputBuffer.data(),
            ret,
            swr.channels,
            swr.sampleRate,
            firstOutputFrame,
            result.error)) {
        return AVERROR_EXIT;
    }
    if (!emitImportProgress(callbacks, result, false, result.error)) {
        return AVERROR_EXIT;
    }
    if (!pollImportCancel(callbacks, result)) {
        return AVERROR_EXIT;
    }
    return 0;
}

int flushStreamingSwr(
    StreamingSwrState& swr,
    std::ofstream& output,
    StreamingStatsAccumulator& stats,
    StreamingImportResult& result,
    StreamingImportCallbacks* callbacks,
    FullScaleClipDiagnostic* diagnostic) {
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
        if (ret < 0) {
            result.error = "swr_convert flush failed: " + ffErrorString(ret);
            return ret;
        }
        if (ret == 0) {
            break;
        }

        const std::size_t writtenSamples = static_cast<std::size_t>(ret) * static_cast<std::size_t>(swr.channels);
        analyzePostSwrSamples(diagnostic, swr.outputBuffer.data(), ret, swr.channels);
        const std::size_t writtenBytes = writtenSamples * sizeof(float);
        output.write(reinterpret_cast<const char*>(swr.outputBuffer.data()), static_cast<std::streamsize>(writtenBytes));
        output.flush();
        if (!output) {
            result.error = "failed to write original_f32.bin";
            return AVERROR(EIO);
        }

        stats.addSamples(swr.outputBuffer.data(), writtenSamples);
        const std::uint64_t firstOutputFrame = nonNegativeToUint64(result.framesWritten);
        std::string countError;
        if (!addWrittenFrames(result, ret, countError)) {
            result.error = countError;
            return AVERROR(EOVERFLOW);
        }
        if (!emitDraftWaveformSamples(
                callbacks,
                swr.outputBuffer.data(),
                ret,
                swr.channels,
                swr.sampleRate,
                firstOutputFrame,
                result.error)) {
            return AVERROR_EXIT;
        }
        if (!emitImportProgress(callbacks, result, false, result.error)) {
            return AVERROR_EXIT;
        }
        if (!pollImportCancel(callbacks, result)) {
            return AVERROR_EXIT;
        }
    }

    return 0;
}

int receiveStreamingFrames(
    AVCodecContext* decoder,
    AVFrame* frame,
    StreamingSwrState& swr,
    std::ofstream& output,
    StreamingStatsAccumulator& stats,
    StreamingImportResult& result,
    StreamingImportCallbacks* callbacks,
    FullScaleClipDiagnostic* diagnostic) {
    while (true) {
        const int ret = avcodec_receive_frame(decoder, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return 0;
        }
        if (ret < 0) {
            addStreamingWarning(result, "avcodec_receive_frame skipped error: " + ffErrorString(ret));
            return 0;
        }

        ++result.decode.decodedFrames;
        const int convertRet =
            writeConvertedStreamingSamples(swr, frame, decoder, output, stats, result, callbacks, diagnostic);
        av_frame_unref(frame);
        if (convertRet < 0) {
            return convertRet;
        }
    }
}

int sendStreamingPacketAndReceive(
    AVCodecContext* decoder,
    AVPacket* packet,
    AVFrame* frame,
    StreamingSwrState& swr,
    std::ofstream& output,
    StreamingStatsAccumulator& stats,
    StreamingImportResult& result,
    StreamingImportCallbacks* callbacks,
    FullScaleClipDiagnostic* diagnostic) {
    int ret = avcodec_send_packet(decoder, packet);
    if (ret == AVERROR(EAGAIN)) {
        ret = receiveStreamingFrames(decoder, frame, swr, output, stats, result, callbacks, diagnostic);
        if (ret < 0) {
            return ret;
        }
        ret = avcodec_send_packet(decoder, packet);
    }

    if (ret < 0) {
        ++result.decode.invalidPacketsSkipped;
        addStreamingWarning(result, "avcodec_send_packet skipped packet: " + ffErrorString(ret));
        return 0;
    }

    return receiveStreamingFrames(decoder, frame, swr, output, stats, result, callbacks, diagnostic);
}

StreamingImportResult runStreamingSessionImportToLiveFile(
    const std::string& path,
    const std::filesystem::path& sessionDir,
    const std::filesystem::path& audioDataPath,
    StreamingImportCallbacks* callbacks) {
    StreamingImportResult result;
    result.source.inputPath = path;

    AVFormatContext* formatContext = nullptr;
    AVCodecContext* decoderContext = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    StreamingSwrState swr;
    StreamingStatsAccumulator stats;
    FullScaleClipDiagnostic fullScaleDiagnostic;
    fullScaleDiagnostic.enabled = fullScaleClipDiagEnabledFromEnvironment();
    fullScaleDiagnostic.path = path;
    std::ofstream output;

    auto cleanup = [&]() {
        if (output.is_open()) {
            output.close();
        }
        if (packet) {
            Ffmpeg::freePacket(packet);
        }
        if (frame) {
            Ffmpeg::freeFrame(frame);
        }
        if (swr.ctx) {
            Ffmpeg::freeSwr(swr.ctx);
        }
        av_channel_layout_uninit(&swr.layout);
        if (decoderContext) {
            Ffmpeg::freeCodecContext(decoderContext);
        }
        if (formatContext) {
            Ffmpeg::closeInput(formatContext);
        }
    };

    int ret = avformat_open_input(&formatContext, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        result.error = "avformat_open_input failed: " + ffErrorString(ret);
        cleanup();
        return result;
    }

    ret = avformat_find_stream_info(formatContext, nullptr);
    if (ret < 0) {
        result.error = "avformat_find_stream_info failed: " + ffErrorString(ret);
        cleanup();
        return result;
    }

    fillStreamingSourceInfo(result, formatContext, path);
    fillStreamingProbeDetails(result, formatContext);

    const Ffmpeg::AudioStreamSelection selection =
        Ffmpeg::selectBestAudioStreamStrict(formatContext);
    if (selection.streamIndex < 0) {
        result.error = "no audio stream found: " + ffErrorString(selection.bestStreamResult);
        cleanup();
        return result;
    }

    const int audioStreamIndex = selection.streamIndex;
    const AVCodec* decoder = selection.decoder;
    AVStream* audioStream = formatContext->streams[audioStreamIndex];
    AVCodecParameters* codecpar = audioStream->codecpar;
    if (!decoder) {
        decoder = avcodec_find_decoder(codecpar->codec_id);
    }
    fillStreamingSelectedAudioInfo(result, audioStream, decoder, nullptr);
    result.diskPreflightEstimateKnown =
        estimateDecodedBytesForPreflight(
            formatContext,
            audioStream,
            path,
            result.preflightEstimatedFrames,
            result.preflightEstimatedBytes,
            result.preflightEstimateKind);

    std::string preflightError;
    if (!checkDiskPreflight(sessionDir, result, preflightError)) {
        result.error = preflightError;
        cleanup();
        return result;
    }

    if (!decoder) {
        result.error = "decoder not found for codec " + result.selectedAudio.codecName;
        cleanup();
        return result;
    }

    decoderContext = avcodec_alloc_context3(decoder);
    if (!decoderContext) {
        result.error = "avcodec_alloc_context3 failed";
        cleanup();
        return result;
    }

    ret = avcodec_parameters_to_context(decoderContext, codecpar);
    if (ret < 0) {
        result.error = "avcodec_parameters_to_context failed: " + ffErrorString(ret);
        cleanup();
        return result;
    }

    if (decoderContext->ch_layout.nb_channels <= 0 && codecpar->ch_layout.nb_channels > 0) {
        av_channel_layout_copy(&decoderContext->ch_layout, &codecpar->ch_layout);
    }

    ret = avcodec_open2(decoderContext, decoder, nullptr);
    if (ret < 0) {
        result.error = "avcodec_open2 failed: " + ffErrorString(ret);
        cleanup();
        return result;
    }
    result.decode.decoderOpened = true;
    fillStreamingSelectedAudioInfo(result, audioStream, decoder, decoderContext);
    if (fullScaleDiagnostic.enabled) {
        fullScaleDiagnostic.codecName = result.selectedAudio.codecName;
        fullScaleDiagnostic.decoderName = result.selectedAudio.decoderName;
    }

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!packet || !frame) {
        result.error = "unable to allocate AVPacket/AVFrame";
        cleanup();
        return result;
    }

    output.open(audioDataPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        result.error = "failed to open original_f32.bin";
        cleanup();
        return result;
    }

    if (!pollImportCancel(callbacks, result)) {
        cleanup();
        return result;
    }

    while ((ret = av_read_frame(formatContext, packet)) >= 0) {
        if (!pollImportCancel(callbacks, result)) {
            av_packet_unref(packet);
            cleanup();
            return result;
        }

        ++result.decode.packetsRead;
        if (packet->stream_index == audioStreamIndex) {
            ++result.decode.audioPackets;
            const int decodeRet =
                sendStreamingPacketAndReceive(
                    decoderContext,
                    packet,
                    frame,
                    swr,
                    output,
                    stats,
                    result,
                    callbacks,
                    &fullScaleDiagnostic);
            av_packet_unref(packet);
            if (decodeRet < 0) {
                cleanup();
                return result;
            }
        } else {
            av_packet_unref(packet);
        }
    }

    if (ret != AVERROR_EOF) {
        result.error = "av_read_frame failed: " + ffErrorString(ret);
        cleanup();
        return result;
    }

    ret = avcodec_send_packet(decoderContext, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        result.error = "decoder flush send failed: " + ffErrorString(ret);
        cleanup();
        return result;
    }

    ret = receiveStreamingFrames(decoderContext, frame, swr, output, stats, result, callbacks, &fullScaleDiagnostic);
    if (ret < 0) {
        cleanup();
        return result;
    }

    ret = flushStreamingSwr(swr, output, stats, result, callbacks, &fullScaleDiagnostic);
    if (ret < 0) {
        cleanup();
        return result;
    }

    if (!flushDraftWaveform(callbacks, result, result.error)) {
        cleanup();
        return result;
    }
    if (!pollImportCancel(callbacks, result)) {
        cleanup();
        return result;
    }

    output.close();
    if (!output) {
        result.error = "failed to finalize original_f32.bin";
        cleanup();
        return result;
    }

    if (result.decode.decodedFrames <= 0 || result.framesWritten <= 0 || result.bytesWritten <= 0) {
        result.error = "no decoded audio samples were produced";
        cleanup();
        return result;
    }
    if (result.sampleRate <= 0 || result.channels <= 0) {
        result.error = "streaming import produced invalid audio shape";
        cleanup();
        return result;
    }

    const std::int64_t maxFramesForExpectedBytes =
        (std::numeric_limits<std::int64_t>::max)() /
        (static_cast<std::int64_t>(result.channels) * static_cast<std::int64_t>(sizeof(float)));
    if (result.framesWritten > maxFramesForExpectedBytes) {
        result.error = "streaming import expected byte count overflow";
        cleanup();
        return result;
    }

    result.expectedBytes =
        result.framesWritten *
        static_cast<std::int64_t>(result.channels) *
        static_cast<std::int64_t>(sizeof(float));
    if (result.bytesWritten != result.expectedBytes) {
        std::ostringstream out;
        out << "streaming import byte count mismatch: expected "
            << result.expectedBytes
            << ", wrote "
            << result.bytesWritten;
        result.error = out.str();
        cleanup();
        return result;
    }

    result.stats = stats.finish();
    if (fullScaleDiagnostic.enabled) {
        if (fullScaleDiagnostic.codecName.empty()) {
            fullScaleDiagnostic.codecName = result.selectedAudio.codecName;
        }
        if (fullScaleDiagnostic.decoderName.empty()) {
            fullScaleDiagnostic.decoderName = result.selectedAudio.decoderName;
        }
        if (fullScaleDiagnostic.inputSampleFormatName.empty()) {
            fullScaleDiagnostic.inputSampleFormatName = result.selectedAudio.decoderSampleFormat;
        }
        logFullScaleClipDiagnostic(fullScaleDiagnostic);
    }
    if (!emitImportProgress(callbacks, result, true, result.error)) {
        cleanup();
        return result;
    }
    result.ok = true;
    cleanup();
    return result;
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

double streamingDurationSeconds(const StreamingImportResult& result) {
    if (result.source.durationSeconds > 0.0) {
        return result.source.durationSeconds;
    }
    if (result.sampleRate > 0 && result.framesWritten > 0) {
        return static_cast<double>(result.framesWritten) / static_cast<double>(result.sampleRate);
    }
    return 0.0;
}

bool streamingHasAudio(const StreamingImportResult& result) {
    return result.ok ||
        result.selectedAudio.index >= 0 ||
        result.probe.audioStreamCount > 0;
}

bool writeStreamingAudioInfoJson(
    const std::filesystem::path& outputPath,
    const StreamingImportResult& result,
    std::string& error) {
    std::ofstream json(outputPath, std::ios::binary);
    if (!json) {
        error = "failed to open audio_info.json";
        return false;
    }

    json << "{\n";
    json << "  \"sampleRate\": " << result.sampleRate << ",\n";
    json << "  \"channels\": " << result.channels << ",\n";
    json << "  \"frames\": " << result.framesWritten << ",\n";
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

bool writeStreamingMetadataJson(
    const std::filesystem::path& outputPath,
    const std::string& sourcePathUtf8,
    const StreamingImportResult& result,
    std::string& error) {
    std::ofstream json(outputPath, std::ios::binary);
    if (!json) {
        error = "failed to open metadata.json";
        return false;
    }

    json << std::fixed << std::setprecision(9);
    json << "{\n";
    json << "  \"apiVersion\": 1,\n";
    json << "  \"sourcePath\": " << jsonString(sourcePathUtf8) << ",\n";
    json << "  \"hasAudio\": " << (streamingHasAudio(result) ? "true" : "false") << ",\n";
    json << "  \"formatName\": " << jsonString(result.source.formatName) << ",\n";
    json << "  \"formatLongName\": " << jsonString(result.source.formatLongName) << ",\n";
    json << "  \"durationSec\": " << streamingDurationSeconds(result) << ",\n";
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
    json << "    \"sampleRate\": " << result.sampleRate << ",\n";
    json << "    \"channels\": " << result.channels << ",\n";
    json << "    \"frames\": " << result.framesWritten << ",\n";
    json << "    \"sampleFormat\": \"float32\",\n";
    json << "    \"sampleLayout\": \"interleaved\",\n";
    json << "    \"dataFile\": \"original_f32.bin\",\n";
    json << "    \"expectedDataBytes\": " << result.expectedBytes << "\n";
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

void removeFileIfExists(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

bool removeFileIfExistsChecked(
    const std::filesystem::path& path,
    const char* label,
    std::string& error) {
    std::error_code fsError;
    std::filesystem::remove(path, fsError);
    if (fsError) {
        error = std::string("failed to remove old ") + label + ": " + fsError.message();
        return false;
    }
    return true;
}

void cleanupStreamingTempJsonFiles(
    const std::filesystem::path& audioInfoTmpPath,
    const std::filesystem::path& metadataTmpPath) {
    removeFileIfExists(audioInfoTmpPath);
    removeFileIfExists(metadataTmpPath);
}

void cleanupStreamingFailedImportArtifacts(
    const std::filesystem::path& audioDataPath,
    const std::filesystem::path& audioInfoTmpPath,
    const std::filesystem::path& metadataTmpPath) {
    removeFileIfExists(audioDataPath);
    cleanupStreamingTempJsonFiles(audioInfoTmpPath, metadataTmpPath);
}

bool prepareStreamingLiveImportArtifacts(
    const std::filesystem::path& audioDataPath,
    const std::filesystem::path& audioInfoPath,
    const std::filesystem::path& metadataPath,
    const std::filesystem::path& audioInfoTmpPath,
    const std::filesystem::path& metadataTmpPath,
    std::string& error) {
    return removeFileIfExistsChecked(audioInfoTmpPath, "audio_info.json.tmp", error) &&
        removeFileIfExistsChecked(metadataTmpPath, "metadata.json.tmp", error) &&
        removeFileIfExistsChecked(audioInfoPath, "audio_info.json", error) &&
        removeFileIfExistsChecked(metadataPath, "metadata.json", error) &&
        removeFileIfExistsChecked(audioDataPath, "original_f32.bin", error);
}

bool writeStreamingImportPlaceholderJson(
    const std::filesystem::path& path,
    const char* label,
    std::string& error) {
    std::ofstream json(path, std::ios::binary | std::ios::trunc);
    if (!json.is_open()) {
        error = std::string("failed to open ") + label;
        return false;
    }
    json << "{\n"
         << "  \"status\": \"importing\"\n"
         << "}\n";
    if (!json) {
        error = std::string("failed to write ") + label;
        return false;
    }
    return true;
}

bool moveFileReplace(const std::filesystem::path& source, const std::filesystem::path& destination, std::string& error) {
    if (MoveFileExW(
            source.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
        return true;
    }

    std::ostringstream out;
    out << "failed to commit "
        << destination.filename().string()
        << ": Win32 error "
        << GetLastError();
    error = out.str();
    return false;
}

bool verifyStreamingDataFileSize(
    const std::filesystem::path& audioDataPath,
    const StreamingImportResult& result,
    std::string& error) {
    std::error_code fsError;
    const auto actualBytes = std::filesystem::file_size(audioDataPath, fsError);
    if (fsError) {
        error = "failed to inspect original_f32.bin: " + fsError.message();
        return false;
    }
    if (actualBytes != static_cast<std::uintmax_t>(result.expectedBytes)) {
        std::ostringstream out;
        out << "streaming import data file size mismatch: expected "
            << result.expectedBytes
            << ", actual "
            << actualBytes;
        error = out.str();
        return false;
    }
    return true;
}

bool commitStreamingSessionArtifacts(
    const std::filesystem::path& audioDataPath,
    const std::filesystem::path& audioInfoTmpPath,
    const std::filesystem::path& metadataTmpPath,
    const std::filesystem::path& audioInfoPath,
    const std::filesystem::path& metadataPath,
    std::string& error) {
    bool infoCommitted = false;
    bool metadataCommitted = false;

    auto rollback = [&]() {
        if (infoCommitted) {
            removeFileIfExists(audioInfoPath);
        }
        if (metadataCommitted) {
            removeFileIfExists(metadataPath);
        }
        removeFileIfExists(audioDataPath);
        cleanupStreamingTempJsonFiles(audioInfoTmpPath, metadataTmpPath);
    };

    std::error_code fsError;
    std::filesystem::remove(metadataPath, fsError);
    if (fsError) {
        error = "failed to remove old metadata.json before commit: " + fsError.message();
        rollback();
        return false;
    }

    if (!moveFileReplace(metadataTmpPath, metadataPath, error)) {
        rollback();
        return false;
    }
    metadataCommitted = true;

    if (!moveFileReplace(audioInfoTmpPath, audioInfoPath, error)) {
        rollback();
        return false;
    }
    infoCommitted = true;

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

        Probe::FastProbeResult probe = Probe::runFastProbe(input);
        if (!Probe::writeFastProbeJson(outputFsPath, probe, error)) {
            setLastErrorText(error);
            return 3;
        }

        if (!probe.document.errors.empty() && !probe.streamInfoFound) {
            setLastErrorText(probe.document.errors.front());
            return 2;
        }

        clearLastError();
        return 0;
    } catch (...) {
        setLastErrorText(L"unexpected exception in AveMediaBridge_ProbeToJson");
        return 99;
    }
}

int AveMediaBridge_ProbeFrameCountCandidatesToJson(
    const wchar_t* inputPath,
    const wchar_t* outputJsonPath) {
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

        Probe::FastProbeResult probe = Probe::runFastProbe(input);
        if (!Probe::writeFastProbeJson(outputFsPath, probe, error)) {
            setLastErrorText(error);
            return 3;
        }

        if (!probe.document.errors.empty() && !probe.streamInfoFound) {
            setLastErrorText(probe.document.errors.front());
            return 2;
        }

        clearLastError();
        return 0;
    } catch (...) {
        setLastErrorText(L"unexpected exception in AveMediaBridge_ProbeFrameCountCandidatesToJson");
        return 99;
    }
}

int AveMediaBridge_ImportAudioToSessionEx(const AveMediaBridgeImportOptions* options) {
    try {
        constexpr std::size_t kImportOptionsBaseSize =
            offsetof(AveMediaBridgeImportOptions, onWaveformChunk);
        constexpr std::size_t kImportOptionsWaveformSize =
            offsetof(AveMediaBridgeImportOptions, onWaveformChunk) +
            sizeof(AveMediaBridgeWaveformChunkCallback);

        if (!options || options->structSize < kImportOptionsBaseSize) {
            setLastErrorText(L"import options are invalid or too small");
            return 1;
        }

        const wchar_t* inputPath = options->inputPath;
        const wchar_t* sessionMediaDir = options->sessionMediaDir;
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

        const std::filesystem::path metadataPath = sessionDir / L"metadata.json";
        const std::filesystem::path audioInfoPath = sessionDir / L"audio_info.json";
        const std::filesystem::path audioDataPath = sessionDir / L"original_f32.bin";
        const std::filesystem::path metadataTmpPath = sessionDir / L"metadata.json.tmp";
        const std::filesystem::path audioInfoTmpPath = sessionDir / L"audio_info.json.tmp";

        if (!prepareStreamingLiveImportArtifacts(
                audioDataPath,
                audioInfoPath,
                metadataPath,
                audioInfoTmpPath,
                metadataTmpPath,
                error)) {
            setLastErrorText(error);
            return 3;
        }
        if (!writeStreamingImportPlaceholderJson(audioInfoTmpPath, "audio_info.json.tmp", error) ||
            !writeStreamingImportPlaceholderJson(metadataTmpPath, "metadata.json.tmp", error)) {
            cleanupStreamingFailedImportArtifacts(audioDataPath, audioInfoTmpPath, metadataTmpPath);
            setLastErrorText(error);
            return 3;
        }

        StreamingImportCallbacks callbacks;
        callbacks.onProgress = options->onProgress;
        callbacks.shouldCancel = options->shouldCancel;
        callbacks.onWaveformChunk =
            options->structSize >= kImportOptionsWaveformSize ? options->onWaveformChunk : nullptr;
        callbacks.userData = options->userData;

        StreamingImportResult imported = runStreamingSessionImportToLiveFile(input, sessionDir, audioDataPath, &callbacks);
        if (!imported.ok) {
            cleanupStreamingFailedImportArtifacts(audioDataPath, audioInfoTmpPath, metadataTmpPath);
            if (imported.canceled) {
                setLastErrorText(imported.error.empty() ? "streaming session import canceled" : imported.error);
                return AVEMEDIABRIDGE_IMPORT_RESULT_CANCELED;
            }
            setLastErrorText(imported.error.empty() ? "streaming audio import failed" : imported.error);
            return 2;
        }

        if (!verifyStreamingDataFileSize(audioDataPath, imported, error) ||
            !writeStreamingAudioInfoJson(audioInfoTmpPath, imported, error) ||
            !writeStreamingMetadataJson(metadataTmpPath, input, imported, error) ||
            !commitStreamingSessionArtifacts(
                audioDataPath,
                audioInfoTmpPath,
                metadataTmpPath,
                audioInfoPath,
                metadataPath,
                error)) {
            cleanupStreamingFailedImportArtifacts(audioDataPath, audioInfoTmpPath, metadataTmpPath);
            setLastErrorText(error);
            return 3;
        }

        clearLastError();
        return 0;
    } catch (...) {
        setLastErrorText(L"unexpected exception in AveMediaBridge_ImportAudioToSessionEx");
        return 99;
    }
}

int AveMediaBridge_ImportAudioToSession(const wchar_t* inputPath, const wchar_t* sessionMediaDir) {
    AveMediaBridgeImportOptions options{};
    options.structSize = sizeof(options);
    options.inputPath = inputPath;
    options.sessionMediaDir = sessionMediaDir;
    return AveMediaBridge_ImportAudioToSessionEx(&options);
}

const wchar_t* AveMediaBridge_GetVersion() {
    return kVersionString;
}
