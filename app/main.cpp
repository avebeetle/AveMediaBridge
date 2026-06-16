#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#define WIN32_LEAN_AND_MEAN

#include "AveMediaBridge/AveMediaBridgeApi.hpp"
#include "AveMediaBridge/AveMediaBridge.hpp"
#include "AveMediaBridge/Core/AudioStats.hpp"
#include "AveMediaBridge/Process/AudioBufferProcessor.hpp"
#include "../src/Diagnostics/FullScaleClipDiagnostics.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#include <libswresample/swresample.h>
}

#ifndef AVEMEDIABRIDGE_PROJECT_ROOT
#define AVEMEDIABRIDGE_PROJECT_ROOT ""
#endif

#ifndef AVEMEDIABRIDGE_FFMPEG_INCLUDE
#define AVEMEDIABRIDGE_FFMPEG_INCLUDE ""
#endif

#ifndef AVEMEDIABRIDGE_FFMPEG_LIB
#define AVEMEDIABRIDGE_FFMPEG_LIB ""
#endif

#ifndef AVEMEDIABRIDGE_FFMPEG_BIN
#define AVEMEDIABRIDGE_FFMPEG_BIN ""
#endif

namespace {

using AveMediaBridge::AudioBufferF32;
using AveMediaBridge::AudioBufferProcessor;
using AveMediaBridge::AudioImportResult;
using AveMediaBridge::AudioStats;

namespace ClipDiag = AveMediaBridge::Diagnostics;

std::string trim(std::string text) {
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), [&](char c) { return !isSpace(static_cast<unsigned char>(c)); }));
    text.erase(std::find_if(text.rbegin(), text.rend(), [&](char c) { return !isSpace(static_cast<unsigned char>(c)); }).base(), text.end());
    return text;
}

std::string stripMatchingQuotes(std::string text) {
    text = trim(std::move(text));
    if (text.size() >= 2 && ((text.front() == '"' && text.back() == '"') || (text.front() == '\'' && text.back() == '\''))) {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

std::wstring stringToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (required <= 0) {
        codePage = CP_ACP;
        flags = 0;
        required = MultiByteToWideChar(codePage, flags, text.c_str(), -1, nullptr, 0);
    }
    if (required <= 1) {
        return {};
    }

    std::wstring wide(static_cast<std::size_t>(required - 1), L'\0');
    MultiByteToWideChar(codePage, flags, text.c_str(), -1, wide.data(), required);
    return wide;
}

std::string lowerString(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

std::filesystem::path getExecutableDirectory() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD copied = 0;
    while (true) {
        copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return {};
        }
        if (copied < buffer.size() - 1) {
            buffer.resize(copied);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }

    return std::filesystem::path(buffer).parent_path();
}

bool hasCommonMediaExtension(const std::filesystem::path& path) {
    const std::string ext = lowerString(path.extension().string());
    static const std::vector<std::string> extensions = {
        ".wav", ".mp3", ".mp4", ".m4a", ".mov", ".flac", ".ogg", ".opus",
        ".aac", ".mkv", ".webm", ".avi", ".aiff", ".aif", ".aifc", ".au",
        ".w64", ".caf", ".mka", ".oga", ".weba", ".nut", ".ts", ".m2ts", ".mts", ".mpg", ".mpeg", ".vob",
        ".ac3", ".eac3", ".ec3", ".asf", ".wma", ".wmv", ".flv", ".f4v",
        ".ogv", ".3gp", ".3g2"
    };
    return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
}

void waitForEnter() {
    std::cout << "\nPress Enter to continue...";
    std::string line;
    std::getline(std::cin, line);
}

void printFfmpegVersions() {
    std::cout << "FFmpeg library versions:\n";
    std::cout << "  avformat   " << avformat_version() << "\n";
    std::cout << "  avcodec    " << avcodec_version() << "\n";
    std::cout << "  avutil     " << avutil_version() << "\n";
    std::cout << "  swresample " << swresample_version() << "\n";
}

void printProjectPaths() {
    std::cout << "Project root: " << AVEMEDIABRIDGE_PROJECT_ROOT << "\n";
    std::cout << "FFmpeg include: " << AVEMEDIABRIDGE_FFMPEG_INCLUDE << "\n";
    std::cout << "FFmpeg lib: " << AVEMEDIABRIDGE_FFMPEG_LIB << "\n";
    std::cout << "FFmpeg bin: " << AVEMEDIABRIDGE_FFMPEG_BIN << "\n";
    std::cout << "Expected FFmpeg DLLs:\n";
    std::cout << "  avformat-*.dll\n";
    std::cout << "  avcodec-*.dll\n";
    std::cout << "  avutil-*.dll\n";
    std::cout << "  swresample-*.dll\n";
}

void printStats(const std::string& label, const AudioStats& stats) {
    std::cout << std::fixed << std::setprecision(6);
    std::cout << label << "\n";
    std::cout << "  RMS: " << stats.rms << "\n";
    std::cout << "  Peak: " << stats.peakAbs << "\n";
    std::cout << "  Min: " << stats.minSample << "\n";
    std::cout << "  Max: " << stats.maxSample << "\n";
    std::cout << "  Clipping risk: " << (stats.clippingRisk ? "yes" : "no") << "\n";
}

void printImportReport(const AudioImportResult& result) {
    if (!result.hasUsableAudio()) {
        std::cout << "Result: FAIL\n";
        std::cout << "Reason: " << (result.error.empty() ? "unknown import failure" : result.error) << "\n";
        return;
    }

    std::cout << "Result: OK\n";
    std::cout << "Input path: " << result.source.inputPath << "\n";
    std::cout << "Format: " << result.source.formatName << " / " << result.source.formatLongName << "\n";
    std::cout << "Stream count: " << result.source.streamCount << "\n";
    std::cout << "Selected audio stream index: " << result.selectedAudio.index << "\n";
    std::cout << "Codec: " << result.selectedAudio.codecName << "\n";
    std::cout << "Decoder: " << result.selectedAudio.decoderName << "\n";
    std::cout << "Output sample rate: " << result.audio.sampleRate << "\n";
    std::cout << "Output channels: " << result.audio.channels << "\n";
    std::cout << "AudioBuffer frames: " << result.audio.frameCount() << "\n";
    std::cout << "AudioBuffer float samples: " << result.audio.sampleCount() << "\n";
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "Duration: " << result.audio.durationSeconds() << " sec\n";
    std::cout << "Consistency: " << (result.audio.isConsistent() ? "ok" : "failed") << "\n";
    std::cout << "Packets read: " << result.decode.packetsRead << "\n";
    std::cout << "Audio packets: " << result.decode.audioPackets << "\n";
    std::cout << "Invalid packets skipped: " << result.decode.invalidPacketsSkipped << "\n";
    std::cout << "Decoded frames: " << result.decode.decodedFrames << "\n";
    std::cout << "Output samples per channel: " << result.decode.outputSamplesPerChannel << "\n";
    std::cout << "Output interleaved float samples: " << result.decode.outputInterleavedFloatSamples << "\n";
    printStats("Audio stats:", result.stats);
    std::cout << "Warnings count: " << result.decode.warningsCount << "\n";
    for (const std::string& warning : result.decode.warnings) {
        std::cout << "Warning: " << warning << "\n";
    }
}

int importOneFile(const std::string& path, bool printFullReport) {
    AveMediaBridge::AveMediaBridge bridge;
    AudioImportResult result = bridge.importAudio(path);
    if (printFullReport) {
        printImportReport(result);
    }

    if (result.hasUsableAudio()) {
        return 0;
    }
    if (result.error.find("no audio stream") != std::string::npos) {
        return 2;
    }
    return 1;
}

using GetLastErrorTextFn = int(__cdecl*)(wchar_t*, int);
using TransformToWavFn = int(__cdecl*)(const wchar_t*, const wchar_t*, float);
using ProbeToJsonFn = int(__cdecl*)(const wchar_t*, const wchar_t*);
using ImportAudioToSessionFn = int(__cdecl*)(const wchar_t*, const wchar_t*);
using ImportAudioToSessionExFn = int(__cdecl*)(const AveMediaBridgeImportOptions*);

struct SessionArtifactSummary {
    bool metadataExists = false;
    bool audioInfoExists = false;
    bool audioDataExists = false;
    std::int64_t sampleRate = 0;
    std::int64_t channels = 0;
    std::int64_t frames = 0;
    std::int64_t expectedBytes = 0;
    std::uintmax_t actualBytes = 0;
    bool sizeMatch = false;
};

struct ConsoleProgressThrottle {
    bool initialized = false;
    std::chrono::steady_clock::time_point startedAt{};
    std::chrono::steady_clock::time_point lastPrintAt{};
    double lastPrintedPercent = -1.0;
};

struct ProgressSmokeState {
    int callbackCount = 0;
    std::uint64_t lastFramesWritten = 0;
    std::uint64_t lastBytesWritten = 0;
    bool monotonic = true;
    bool cancelAfterFirstProgress = false;
    bool cancelRequested = false;
    bool liveReadableCheck = false;
    bool firstPositiveProgressChecked = false;
    bool firstPositiveAudioExists = false;
    bool firstPositiveSizeOk = false;
    bool firstPositiveTempJsonExists = false;
    bool liveFileSizeNeverBehind = true;
    std::filesystem::path sessionMediaDir;
    ConsoleProgressThrottle console;
};

struct WaveformChunkSmokeState {
    int progressCallbackCount = 0;
    int waveformChunkCount = 0;
    std::uint64_t lastFramesWritten = 0;
    std::uint64_t lastBytesWritten = 0;
    bool progressMonotonic = true;
    std::uint64_t firstFrameFirst = 0;
    std::uint64_t firstFrameLast = 0;
    std::uint64_t lastFirstFrame = 0;
    std::uint64_t totalBinsReceived = 0;
    bool firstFrameMonotonic = true;
    bool binCountValid = true;
    bool valuesPerBinValid = true;
    bool minMaxPairsValid = true;
    bool longFormEnergyObserved = false;
    bool longFormEnergyValid = true;
    std::uint64_t energyFrameCountTotal = 0;
    ConsoleProgressThrottle console;
};

std::uint32_t importOptionsSizeBeforeWaveformCallback() {
    return static_cast<std::uint32_t>(offsetof(AveMediaBridgeImportOptions, onWaveformChunk));
}

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(std::wstring name, std::wstring value)
        : name_(std::move(name)) {
        std::wstring buffer(32767, L'\0');
        const DWORD copied =
            GetEnvironmentVariableW(name_.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied > 0 && copied < buffer.size()) {
            hadOldValue_ = true;
            buffer.resize(copied);
            oldValue_ = std::move(buffer);
        }
        setOk_ = SetEnvironmentVariableW(name_.c_str(), value.c_str()) != 0;
    }

    ~ScopedEnvironmentVariable() {
        if (name_.empty()) {
            return;
        }
        SetEnvironmentVariableW(name_.c_str(), hadOldValue_ ? oldValue_.c_str() : nullptr);
    }

    bool ok() const {
        return setOk_;
    }

private:
    std::wstring name_;
    std::wstring oldValue_;
    bool hadOldValue_ = false;
    bool setOk_ = false;
};

bool createFullScaleDiagSessionDir(std::filesystem::path& sessionDir, std::string& error) {
    std::error_code fsError;
    std::filesystem::path baseDir = std::filesystem::temp_directory_path(fsError);
    if (fsError) {
        baseDir = std::filesystem::current_path(fsError) / "test_output";
    }
    if (fsError) {
        error = "unable to choose temporary diagnostic session directory: " + fsError.message();
        return false;
    }

    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream name;
    name << "AveMediaBridge_full_scale_clip_diag_"
         << GetCurrentProcessId()
         << "_"
         << ticks;
    sessionDir = baseDir / name.str();
    std::filesystem::create_directories(sessionDir, fsError);
    if (fsError) {
        error = "unable to create diagnostic session directory: " + fsError.message();
        return false;
    }
    return true;
}

void configureDllSearchPath(const std::filesystem::path& exeDir, const std::filesystem::path& ffmpegDir) {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    AddDllDirectory(exeDir.c_str());
    AddDllDirectory(ffmpegDir.c_str());
}

HMODULE loadBridgeDll(const std::filesystem::path& exeDir, const std::filesystem::path& ffmpegDir) {
    configureDllSearchPath(exeDir, ffmpegDir);
    return LoadLibraryW(L"AveMediaBridge.dll");
}

std::wstring getDllLastErrorText(HMODULE module) {
    if (!module) {
        return {};
    }

    auto getLastErrorText = reinterpret_cast<GetLastErrorTextFn>(GetProcAddress(module, "AveMediaBridge_GetLastErrorText"));
    if (!getLastErrorText) {
        return {};
    }

    wchar_t buffer[2048] = {};
    const int result = getLastErrorText(buffer, static_cast<int>(sizeof(buffer) / sizeof(buffer[0])));
    if ((result == 0 || result == 2) && buffer[0] != L'\0') {
        return buffer;
    }
    return {};
}

void printDllLastErrorText(HMODULE module) {
    const std::wstring text = getDllLastErrorText(module);
    if (!text.empty()) {
        std::wcerr << L"DLL last error: " << text << L"\n";
    }
}

bool readTextFile(const std::filesystem::path& path, std::string& text) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    text.assign(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
    return true;
}

bool extractJsonInt64(const std::string& json, const std::string& key, std::int64_t& value) {
    const std::string token = "\"" + key + "\"";
    std::size_t pos = json.find(token);
    if (pos == std::string::npos) {
        return false;
    }
    pos = json.find(':', pos + token.size());
    if (pos == std::string::npos) {
        return false;
    }
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])) != 0) {
        ++pos;
    }

    const std::size_t numberStart = pos;
    if (pos < json.size() && json[pos] == '-') {
        ++pos;
    }
    while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos])) != 0) {
        ++pos;
    }
    if (pos == numberStart || (pos == numberStart + 1 && json[numberStart] == '-')) {
        return false;
    }

    try {
        value = std::stoll(json.substr(numberStart, pos - numberStart));
    } catch (...) {
        return false;
    }
    return true;
}

bool readSessionArtifactSummary(const std::filesystem::path& sessionPath, SessionArtifactSummary& summary, std::string& error) {
    const std::filesystem::path metadataPath = sessionPath / "metadata.json";
    const std::filesystem::path audioInfoPath = sessionPath / "audio_info.json";
    const std::filesystem::path audioDataPath = sessionPath / "original_f32.bin";

    std::error_code fsError;
    summary.metadataExists = std::filesystem::exists(metadataPath, fsError);
    if (fsError) {
        error = "unable to inspect metadata.json: " + fsError.message();
        return false;
    }
    summary.audioInfoExists = std::filesystem::exists(audioInfoPath, fsError);
    if (fsError) {
        error = "unable to inspect audio_info.json: " + fsError.message();
        return false;
    }
    summary.audioDataExists = std::filesystem::exists(audioDataPath, fsError);
    if (fsError) {
        error = "unable to inspect original_f32.bin: " + fsError.message();
        return false;
    }

    if (!summary.audioInfoExists || !summary.audioDataExists) {
        summary.sizeMatch = false;
        return true;
    }

    std::string audioInfoText;
    if (!readTextFile(audioInfoPath, audioInfoText) ||
        !extractJsonInt64(audioInfoText, "sampleRate", summary.sampleRate) ||
        !extractJsonInt64(audioInfoText, "channels", summary.channels) ||
        !extractJsonInt64(audioInfoText, "frames", summary.frames)) {
        error = "unable to read generated audio_info.json summary";
        return false;
    }

    summary.actualBytes = std::filesystem::file_size(audioDataPath, fsError);
    if (fsError) {
        error = "unable to inspect original_f32.bin size: " + fsError.message();
        return false;
    }

    summary.expectedBytes = summary.frames * summary.channels * static_cast<std::int64_t>(sizeof(float));
    summary.sizeMatch = summary.actualBytes == static_cast<std::uintmax_t>(summary.expectedBytes);
    return true;
}

void printSessionArtifactSummary(const std::string& sessionMediaDirText, const SessionArtifactSummary& summary) {
    std::cout << "Session media dir: " << sessionMediaDirText << "\n";
    std::cout << "Streaming import: yes\n";
    std::cout << "Sample rate: " << summary.sampleRate << "\n";
    std::cout << "Channels: " << summary.channels << "\n";
    std::cout << "Frames: " << summary.frames << "\n";
    std::cout << "Expected bytes: " << summary.expectedBytes << "\n";
    std::cout << "Actual original_f32.bin bytes: " << summary.actualBytes << "\n";
    std::cout << "Size match: " << (summary.sizeMatch ? "yes" : "no") << "\n";
    std::cout << "Generated:\n";
    std::cout << "  metadata.json " << (summary.metadataExists ? "yes" : "missing") << "\n";
    std::cout << "  audio_info.json " << (summary.audioInfoExists ? "yes" : "missing") << "\n";
    std::cout << "  original_f32.bin " << (summary.audioDataExists ? "yes" : "missing") << "\n";
}

std::string progressPercentText(double progress01) {
    if (progress01 < 0.0 || !std::isfinite(progress01)) {
        return "unknown";
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << (std::clamp(progress01, 0.0, 1.0) * 100.0) << "%";
    return out.str();
}

std::string secondsProgressText(double seconds) {
    if (seconds < 0.0 || !std::isfinite(seconds)) {
        return "unknown";
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << seconds << "s";
    return out.str();
}

std::string megabytesText(std::uint64_t bytes) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2)
        << (static_cast<double>(bytes) / (1024.0 * 1024.0))
        << " MB";
    return out.str();
}

void printThrottledConsoleProgress(
    ConsoleProgressThrottle& throttle,
    const AveMediaBridgeImportProgress& progress,
    int waveformChunkCount,
    bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!throttle.initialized) {
        throttle.initialized = true;
        throttle.startedAt = now;
        throttle.lastPrintAt = now - std::chrono::seconds(1);
    }

    const double percent = progress.progress01 >= 0.0 && std::isfinite(progress.progress01)
        ? std::clamp(progress.progress01 * 100.0, 0.0, 100.0)
        : -1.0;
    const bool percentChanged =
        percent >= 0.0 &&
        (throttle.lastPrintedPercent < 0.0 || percent - throttle.lastPrintedPercent >= 1.0);
    const bool timeElapsed = now - throttle.lastPrintAt >= std::chrono::milliseconds(350);
    if (!force && !percentChanged && !timeElapsed) {
        return;
    }

    const double elapsedSec =
        std::chrono::duration<double>(now - throttle.startedAt).count();
    const double mbWritten = static_cast<double>(progress.bytesWritten) / (1024.0 * 1024.0);
    const double mbPerSec = elapsedSec > 0.0 ? mbWritten / elapsedSec : 0.0;
    const double estimatedDurationSec =
        progress.sampleRate > 0 && progress.estimatedTotalFrames > 0
            ? static_cast<double>(progress.estimatedTotalFrames) / static_cast<double>(progress.sampleRate)
            : -1.0;

    std::cout << "Progress: " << progressPercentText(progress.progress01)
              << " time " << secondsProgressText(progress.availableEndSec)
              << " / " << secondsProgressText(estimatedDurationSec)
              << " frames=" << progress.framesWritten
              << " written=" << megabytesText(progress.bytesWritten)
              << " waveformChunks=" << waveformChunkCount
              << " speed=" << std::fixed << std::setprecision(2) << mbPerSec << " MB/s";
    if ((progress.flags & 1u) != 0u) {
        std::cout << " final";
    }
    std::cout << "\n";

    throttle.lastPrintAt = now;
    if (percent >= 0.0) {
        throttle.lastPrintedPercent = percent;
    }
}

bool fileExistsNoThrow(const std::filesystem::path& path) {
    std::error_code fsError;
    const bool exists = std::filesystem::exists(path, fsError);
    return exists && !fsError;
}

bool fileSizeAtLeast(const std::filesystem::path& path, std::uint64_t bytes) {
    std::error_code fsError;
    const auto size = std::filesystem::file_size(path, fsError);
    return !fsError && size >= static_cast<std::uintmax_t>(bytes);
}

bool sessionTempJsonFilesExist(const std::filesystem::path& sessionPath) {
    return fileExistsNoThrow(sessionPath / "audio_info.json.tmp") ||
        fileExistsNoThrow(sessionPath / "metadata.json.tmp");
}

void AVEMEDIABRIDGE_CALL printProgressSmokeCallback(
    const AveMediaBridgeImportProgress* progress,
    void* userData) {
    auto* state = static_cast<ProgressSmokeState*>(userData);
    if (!progress || !state) {
        return;
    }

    if (state->callbackCount > 0 && progress->framesWritten < state->lastFramesWritten) {
        state->monotonic = false;
    }
    state->lastFramesWritten = progress->framesWritten;
    state->lastBytesWritten = progress->bytesWritten;
    ++state->callbackCount;

    if (state->liveReadableCheck && progress->framesWritten > 0) {
        const std::filesystem::path audioPath = state->sessionMediaDir / "original_f32.bin";
        const bool exists = fileExistsNoThrow(audioPath);
        const bool sizeOk = exists && fileSizeAtLeast(audioPath, progress->bytesWritten);
        const bool tempJsonExists =
            fileExistsNoThrow(state->sessionMediaDir / "audio_info.json.tmp") &&
            fileExistsNoThrow(state->sessionMediaDir / "metadata.json.tmp");
        if (!state->firstPositiveProgressChecked) {
            state->firstPositiveProgressChecked = true;
            state->firstPositiveAudioExists = exists;
            state->firstPositiveSizeOk = sizeOk;
            state->firstPositiveTempJsonExists = tempJsonExists;
            std::cout << "Live-readable first progress: original_f32.bin exists="
                      << (exists ? "yes" : "no")
                      << " size>=bytesWritten=" << (sizeOk ? "yes" : "no")
                      << " tmpJsonsExist=" << (tempJsonExists ? "yes" : "no")
                      << "\n";
        }
        if (!sizeOk) {
            state->liveFileSizeNeverBehind = false;
        }
    }

    printThrottledConsoleProgress(
        state->console,
        *progress,
        0,
        (progress->flags & 1u) != 0u);

    if (state->cancelAfterFirstProgress && progress->framesWritten > 0 && (progress->flags & 1u) == 0u) {
        state->cancelRequested = true;
    }
}

int AVEMEDIABRIDGE_CALL cancelSmokeCallback(void* userData) {
    auto* state = static_cast<ProgressSmokeState*>(userData);
    return state && state->cancelRequested ? 1 : 0;
}

void AVEMEDIABRIDGE_CALL waveformProgressSmokeCallback(
    const AveMediaBridgeImportProgress* progress,
    void* userData) {
    auto* state = static_cast<WaveformChunkSmokeState*>(userData);
    if (!progress || !state) {
        return;
    }

    ++state->progressCallbackCount;
    if (state->progressCallbackCount > 1 && progress->framesWritten < state->lastFramesWritten) {
        state->progressMonotonic = false;
    }
    state->lastFramesWritten = progress->framesWritten;
    state->lastBytesWritten = progress->bytesWritten;
    printThrottledConsoleProgress(
        state->console,
        *progress,
        state->waveformChunkCount,
        (progress->flags & 1u) != 0u);
}

void AVEMEDIABRIDGE_CALL waveformChunkSmokeCallback(
    const AveMediaBridgeWaveformChunk* chunk,
    void* userData) {
    auto* state = static_cast<WaveformChunkSmokeState*>(userData);
    if (!chunk || !state) {
        return;
    }

    if (state->waveformChunkCount == 0) {
        state->firstFrameFirst = chunk->firstFrame;
    } else if (chunk->firstFrame < state->lastFirstFrame) {
        state->firstFrameMonotonic = false;
    }
    state->lastFirstFrame = chunk->firstFrame;
    state->firstFrameLast = chunk->firstFrame;
    ++state->waveformChunkCount;

    if (chunk->binCount == 0) {
        state->binCountValid = false;
    }
    if (chunk->valuesPerBin != 2) {
        state->valuesPerBinValid = false;
    }
    if (!chunk->minMaxPairs) {
        state->minMaxPairsValid = false;
    } else {
        const std::size_t valueCount =
            static_cast<std::size_t>(chunk->binCount) * static_cast<std::size_t>(chunk->valuesPerBin);
        for (std::size_t i = 0; i < valueCount; ++i) {
            if (!std::isfinite(chunk->minMaxPairs[i])) {
                state->minMaxPairsValid = false;
                break;
            }
        }
    }
    const bool hasEnergy =
        chunk->structSize >= offsetof(AveMediaBridgeWaveformChunk, frameCountPerBin) +
                sizeof(chunk->frameCountPerBin) &&
        (chunk->flags & AVEMEDIABRIDGE_WAVEFORM_CHUNK_FLAG_LONG_FORM_ENERGY) != 0 &&
        chunk->sumSquaresPerBin &&
        chunk->sumAbsPerBin &&
        chunk->frameCountPerBin;
    if (hasEnergy) {
        state->longFormEnergyObserved = true;
        for (std::uint32_t bin = 0; bin < chunk->binCount; ++bin) {
            if (!std::isfinite(chunk->sumSquaresPerBin[bin]) ||
                !std::isfinite(chunk->sumAbsPerBin[bin]) ||
                chunk->frameCountPerBin[bin] == 0 ||
                chunk->frameCountPerBin[bin] > chunk->framesPerBin) {
                state->longFormEnergyValid = false;
                break;
            }
            state->energyFrameCountTotal += chunk->frameCountPerBin[bin];
        }
    }
    state->totalBinsReceived += chunk->binCount;
}

struct SyntheticEnergyBin {
    float minValue = 0.0f;
    float maxValue = 0.0f;
    double sumSquares = 0.0;
    double sumAbs = 0.0;
    std::uint64_t frameCount = 0;
};

std::vector<SyntheticEnergyBin> buildSyntheticEnergyBins(
    const std::vector<float>& monoSamples,
    std::uint32_t framesPerBin) {
    std::vector<SyntheticEnergyBin> bins;
    if (framesPerBin == 0) {
        return bins;
    }

    SyntheticEnergyBin current;
    bool currentHasSamples = false;
    std::uint32_t currentFrames = 0;
    for (float sample : monoSamples) {
        const float mono = std::isfinite(sample) ? sample : 0.0f;
        if (!currentHasSamples) {
            current.minValue = mono;
            current.maxValue = mono;
            currentHasSamples = true;
        } else {
            current.minValue = (std::min)(current.minValue, mono);
            current.maxValue = (std::max)(current.maxValue, mono);
        }
        current.sumSquares += static_cast<double>(mono) * static_cast<double>(mono);
        current.sumAbs += std::abs(static_cast<double>(mono));
        ++current.frameCount;
        ++currentFrames;

        if (currentFrames == framesPerBin) {
            bins.push_back(current);
            current = {};
            currentHasSamples = false;
            currentFrames = 0;
        }
    }

    if (currentHasSamples) {
        bins.push_back(current);
    }
    return bins;
}

double syntheticRms(const SyntheticEnergyBin& bin) {
    return bin.frameCount > 0
        ? std::sqrt(bin.sumSquares / static_cast<double>(bin.frameCount))
        : 0.0;
}

double syntheticMeanAbs(const SyntheticEnergyBin& bin) {
    return bin.frameCount > 0
        ? bin.sumAbs / static_cast<double>(bin.frameCount)
        : 0.0;
}

int runWaveformEnergyChunkContractTest() {
    auto nearEnough = [](double left, double right) {
        return std::abs(left - right) <= 0.000001;
    };

    const auto silence = buildSyntheticEnergyBins(std::vector<float>(4, 0.0f), 4);
    const auto constant = buildSyntheticEnergyBins(std::vector<float>(4, 0.5f), 4);
    std::vector<float> transientSamples(128, 0.0f);
    transientSamples[127] = 1.0f;
    const auto transient = buildSyntheticEnergyBins(transientSamples, 128);
    const auto tail = buildSyntheticEnergyBins({0.25f, 0.25f, 0.25f, 0.25f, 0.75f}, 4);

    const bool silenceOk =
        silence.size() == 1 &&
        silence[0].minValue == 0.0f &&
        silence[0].maxValue == 0.0f &&
        nearEnough(syntheticRms(silence[0]), 0.0) &&
        nearEnough(syntheticMeanAbs(silence[0]), 0.0);
    const bool constantOk =
        constant.size() == 1 &&
        constant[0].minValue == 0.5f &&
        constant[0].maxValue == 0.5f &&
        nearEnough(syntheticRms(constant[0]), 0.5) &&
        nearEnough(syntheticMeanAbs(constant[0]), 0.5);
    const bool transientOk =
        transient.size() == 1 &&
        transient[0].maxValue == 1.0f &&
        syntheticRms(transient[0]) < 0.1 &&
        syntheticMeanAbs(transient[0]) < 0.01;
    const bool tailOk =
        tail.size() == 2 &&
        tail[0].frameCount == 4 &&
        tail[1].frameCount == 1 &&
        tail[1].minValue == 0.75f &&
        tail[1].maxValue == 0.75f;

    std::cout << "LongForm energy chunk contract test\n";
    std::cout << "  silence: " << (silenceOk ? "ok" : "failed") << "\n";
    std::cout << "  constant 0.5 RMS: " << syntheticRms(constant[0]) << "\n";
    std::cout << "  constant 0.5 meanAbs: " << syntheticMeanAbs(constant[0]) << "\n";
    std::cout << "  transient peak: " << transient[0].maxValue << "\n";
    std::cout << "  transient RMS: " << syntheticRms(transient[0]) << "\n";
    std::cout << "  transient meanAbs: " << syntheticMeanAbs(transient[0]) << "\n";
    std::cout << "  tail frameCount: " << tail[1].frameCount << "\n";
    std::cout << "  flag: AVEMEDIABRIDGE_WAVEFORM_CHUNK_FLAG_LONG_FORM_ENERGY\n";

    if (!silenceOk || !constantOk || !transientOk || !tailOk) {
        std::cout << "Runtime status: AVEMEDIABRIDGE_WAVEFORM_ENERGY_CHUNK_FAILED\n";
        return 1;
    }

    std::cout << "Runtime status: AVEMEDIABRIDGE_WAVEFORM_ENERGY_CHUNK_OK\n";
    return 0;
}

bool analyzeMonoFrameSamples(
    AVSampleFormat format,
    void* sampleData,
    int sampleCount,
    ClipDiag::FullScaleSampleStats& stats) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        return false;
    }

    frame->format = format;
    frame->nb_samples = sampleCount;
    av_channel_layout_default(&frame->ch_layout, 1);
    frame->data[0] = static_cast<std::uint8_t*>(sampleData);
    frame->extended_data = frame->data;
    const bool ok = ClipDiag::analyzeFrameSamples(frame, 1, stats);
    av_channel_layout_uninit(&frame->ch_layout);
    av_frame_free(&frame);
    return ok;
}

int runFullScaleSampleStatsContractTest() {
    auto nearEnough = [](double left, double right) {
        return std::abs(left - right) <= 0.000001;
    };

    std::int16_t s16Samples[] = {
        static_cast<std::int16_t>(-32768),
        static_cast<std::int16_t>(0),
        static_cast<std::int16_t>(32767)
    };
    ClipDiag::FullScaleSampleStats s16Stats;
    const bool s16Analyzed =
        analyzeMonoFrameSamples(AV_SAMPLE_FMT_S16, s16Samples, 3, s16Stats);
    const bool s16Ok =
        s16Analyzed &&
        s16Stats.sampleCount == 3 &&
        s16Stats.finiteCount == 3 &&
        s16Stats.exactNegativeOneCount == 1 &&
        s16Stats.exactPositiveOneCount == 0 &&
        s16Stats.nearNegativeOneCount == 1 &&
        s16Stats.nearPositiveOneCount == 0 &&
        s16Stats.abovePositiveOneCount == 0 &&
        s16Stats.belowNegativeOneCount == 0 &&
        s16Stats.zeroCount == 1 &&
        nearEnough(s16Stats.minValue, -1.0) &&
        nearEnough(s16Stats.maxValue, 32767.0 / 32768.0) &&
        nearEnough(s16Stats.maxAbs, 1.0);

    float floatSamples[] = {-1.2f, -1.0f, 0.0f, 1.0f, 1.25f};
    ClipDiag::FullScaleSampleStats floatStats;
    const bool floatAnalyzed =
        analyzeMonoFrameSamples(AV_SAMPLE_FMT_FLT, floatSamples, 5, floatStats);
    const bool floatOk =
        floatAnalyzed &&
        floatStats.sampleCount == 5 &&
        floatStats.finiteCount == 5 &&
        floatStats.exactNegativeOneCount == 1 &&
        floatStats.exactPositiveOneCount == 1 &&
        floatStats.nearNegativeOneCount == 1 &&
        floatStats.nearPositiveOneCount == 1 &&
        floatStats.abovePositiveOneCount == 1 &&
        floatStats.belowNegativeOneCount == 1 &&
        floatStats.zeroCount == 1 &&
        nearEnough(floatStats.minValue, -1.2) &&
        nearEnough(floatStats.maxValue, 1.25) &&
        nearEnough(floatStats.maxAbs, 1.25);

    std::cout << "Full-scale sample stats contract test\n";
    std::cout << "  S16 analyzed: " << (s16Analyzed ? "yes" : "no") << "\n";
    std::cout << "  S16 exact -1: " << s16Stats.exactNegativeOneCount << "\n";
    std::cout << "  S16 exact +1: " << s16Stats.exactPositiveOneCount << "\n";
    std::cout << "  FLT analyzed: " << (floatAnalyzed ? "yes" : "no") << "\n";
    std::cout << "  FLT exact -1: " << floatStats.exactNegativeOneCount << "\n";
    std::cout << "  FLT exact +1: " << floatStats.exactPositiveOneCount << "\n";
    std::cout << "  FLT > +1: " << floatStats.abovePositiveOneCount << "\n";
    std::cout << "  FLT < -1: " << floatStats.belowNegativeOneCount << "\n";

    if (!s16Ok || !floatOk) {
        std::cout << "Runtime status: AVEMEDIABRIDGE_FULL_SCALE_STATS_CONTRACT_FAILED\n";
        return 1;
    }

    std::cout << "Runtime status: AVEMEDIABRIDGE_FULL_SCALE_STATS_CONTRACT_OK\n";
    return 0;
}

int runSmokeDllTransform(const std::string& inputPathText, const std::string& outputPathText) {
    const std::filesystem::path exeDir = getExecutableDirectory();
    const std::filesystem::path ffmpegDir = exeDir / "Lib" / "ffmpeg";
    const std::filesystem::path dllPath = exeDir / "AveMediaBridge.dll";
    const std::filesystem::path outputPath = std::filesystem::path(outputPathText);

    std::error_code fsError;
    if (!outputPath.parent_path().empty()) {
        std::filesystem::create_directories(outputPath.parent_path(), fsError);
        if (fsError) {
            std::cerr << "ERROR: failed to create output directory: " << fsError.message() << "\n";
            return 1;
        }
    }

    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    AddDllDirectory(exeDir.c_str());
    AddDllDirectory(ffmpegDir.c_str());

    std::wcout << L"Smoke DLL transform\n";
    std::wcout << L"  DLL: " << dllPath.wstring() << L"\n";
    std::wcout << L"  FFmpeg DLL dir: " << ffmpegDir.wstring() << L"\n";

    HMODULE module = loadBridgeDll(exeDir, ffmpegDir);
    if (!module) {
        std::wcerr << L"ERROR: LoadLibraryW(AveMediaBridge.dll) failed. Win32 error: " << GetLastError() << L"\n";
        return 1;
    }

    auto transform = reinterpret_cast<TransformToWavFn>(GetProcAddress(module, "AveMediaBridge_TransformToWav"));
    if (!transform) {
        std::cerr << "ERROR: GetProcAddress(AveMediaBridge_TransformToWav) failed. Win32 error: " << GetLastError() << "\n";
        FreeLibrary(module);
        return 1;
    }

    const std::wstring inputPath = stringToWide(inputPathText);
    const std::wstring outputWavPath = stringToWide(outputPathText);
    if (inputPath.empty() || outputWavPath.empty()) {
        std::cerr << "ERROR: unable to convert input or output path to UTF-16.\n";
        FreeLibrary(module);
        return 1;
    }

    const int result = transform(inputPath.c_str(), outputWavPath.c_str(), 0.5f);
    if (result != 0) {
        std::cerr << "Result: FAIL\n";
        std::cerr << "AveMediaBridge_TransformToWav returned " << result << "\n";
        printDllLastErrorText(module);
        FreeLibrary(module);
        return result;
    }

    FreeLibrary(module);
    std::cout << "Result: OK\n";
    std::cout << "Output WAV: " << outputPathText << "\n";
    return 0;
}

int runSmokeDllProbe(const std::string& inputPathText, const std::string& outputJsonPathText) {
    const std::filesystem::path exeDir = getExecutableDirectory();
    const std::filesystem::path ffmpegDir = exeDir / "Lib" / "ffmpeg";
    const std::filesystem::path dllPath = exeDir / "AveMediaBridge.dll";
    const std::filesystem::path outputPath = std::filesystem::path(outputJsonPathText);

    std::error_code fsError;
    if (!outputPath.parent_path().empty()) {
        std::filesystem::create_directories(outputPath.parent_path(), fsError);
        if (fsError) {
            std::cerr << "ERROR: failed to create output directory: " << fsError.message() << "\n";
            return 1;
        }
    }

    std::wcout << L"Smoke DLL probe\n";
    std::wcout << L"  DLL: " << dllPath.wstring() << L"\n";
    std::wcout << L"  FFmpeg DLL dir: " << ffmpegDir.wstring() << L"\n";

    HMODULE module = loadBridgeDll(exeDir, ffmpegDir);
    if (!module) {
        std::wcerr << L"ERROR: LoadLibraryW(AveMediaBridge.dll) failed. Win32 error: " << GetLastError() << L"\n";
        return 1;
    }

    auto probe = reinterpret_cast<ProbeToJsonFn>(GetProcAddress(module, "AveMediaBridge_ProbeToJson"));
    if (!probe) {
        std::cerr << "ERROR: GetProcAddress(AveMediaBridge_ProbeToJson) failed. Win32 error: " << GetLastError() << "\n";
        FreeLibrary(module);
        return 1;
    }

    const std::wstring inputPath = stringToWide(inputPathText);
    const std::wstring outputJsonPath = stringToWide(outputJsonPathText);
    if (inputPath.empty() || outputJsonPath.empty()) {
        std::cerr << "ERROR: unable to convert input or output path to UTF-16.\n";
        FreeLibrary(module);
        return 1;
    }

    const int result = probe(inputPath.c_str(), outputJsonPath.c_str());
    if (result != 0) {
        std::cerr << "Result: FAIL\n";
        std::cerr << "AveMediaBridge_ProbeToJson returned " << result << "\n";
        printDllLastErrorText(module);
        FreeLibrary(module);
        return result;
    }

    FreeLibrary(module);
    std::cout << "Result: OK\n";
    std::cout << "Output JSON: " << outputJsonPathText << "\n";
    return 0;
}

int runSmokeDllImportSession(const std::string& inputPathText, const std::string& sessionMediaDirText) {
    const std::filesystem::path exeDir = getExecutableDirectory();
    const std::filesystem::path ffmpegDir = exeDir / "Lib" / "ffmpeg";
    const std::filesystem::path dllPath = exeDir / "AveMediaBridge.dll";

    std::wcout << L"Smoke DLL import session\n";
    std::wcout << L"  DLL: " << dllPath.wstring() << L"\n";
    std::wcout << L"  FFmpeg DLL dir: " << ffmpegDir.wstring() << L"\n";

    HMODULE module = loadBridgeDll(exeDir, ffmpegDir);
    if (!module) {
        std::wcerr << L"ERROR: LoadLibraryW(AveMediaBridge.dll) failed. Win32 error: " << GetLastError() << L"\n";
        return 1;
    }

    auto importSession = reinterpret_cast<ImportAudioToSessionFn>(GetProcAddress(module, "AveMediaBridge_ImportAudioToSession"));
    if (!importSession) {
        std::cerr << "ERROR: GetProcAddress(AveMediaBridge_ImportAudioToSession) failed. Win32 error: " << GetLastError() << "\n";
        FreeLibrary(module);
        return 1;
    }

    const std::wstring inputPath = stringToWide(inputPathText);
    const std::wstring sessionMediaDir = stringToWide(sessionMediaDirText);
    if (inputPath.empty() || sessionMediaDir.empty()) {
        std::cerr << "ERROR: unable to convert input or session path to UTF-16.\n";
        FreeLibrary(module);
        return 1;
    }

    const int result = importSession(inputPath.c_str(), sessionMediaDir.c_str());
    if (result != 0) {
        std::cerr << "Result: FAIL\n";
        std::cerr << "AveMediaBridge_ImportAudioToSession returned " << result << "\n";
        printDllLastErrorText(module);
        FreeLibrary(module);
        return result;
    }

    FreeLibrary(module);
    const std::filesystem::path sessionPath(sessionMediaDirText);
    SessionArtifactSummary summary;
    std::string summaryError;
    if (!readSessionArtifactSummary(sessionPath, summary, summaryError)) {
        std::cerr << "ERROR: " << summaryError << ".\n";
        return 1;
    }

    std::cout << "Result: OK\n";
    printSessionArtifactSummary(sessionMediaDirText, summary);
    return summary.metadataExists && summary.audioInfoExists && summary.audioDataExists && summary.sizeMatch ? 0 : 1;
}

int runSmokeDllImportSessionProgress(const std::string& inputPathText, const std::string& sessionMediaDirText) {
    const std::filesystem::path exeDir = getExecutableDirectory();
    const std::filesystem::path ffmpegDir = exeDir / "Lib" / "ffmpeg";
    const std::filesystem::path dllPath = exeDir / "AveMediaBridge.dll";

    std::wcout << L"Smoke DLL import session progress\n";
    std::wcout << L"  DLL: " << dllPath.wstring() << L"\n";
    std::wcout << L"  FFmpeg DLL dir: " << ffmpegDir.wstring() << L"\n";

    HMODULE module = loadBridgeDll(exeDir, ffmpegDir);
    if (!module) {
        std::wcerr << L"ERROR: LoadLibraryW(AveMediaBridge.dll) failed. Win32 error: " << GetLastError() << L"\n";
        return 1;
    }

    auto importSessionEx =
        reinterpret_cast<ImportAudioToSessionExFn>(GetProcAddress(module, "AveMediaBridge_ImportAudioToSessionEx"));
    if (!importSessionEx) {
        std::cerr << "ERROR: GetProcAddress(AveMediaBridge_ImportAudioToSessionEx) failed. Win32 error: " << GetLastError() << "\n";
        FreeLibrary(module);
        return 1;
    }

    const std::wstring inputPath = stringToWide(inputPathText);
    const std::wstring sessionMediaDir = stringToWide(sessionMediaDirText);
    if (inputPath.empty() || sessionMediaDir.empty()) {
        std::cerr << "ERROR: unable to convert input or session path to UTF-16.\n";
        FreeLibrary(module);
        return 1;
    }

    ProgressSmokeState progressState;
    progressState.sessionMediaDir = std::filesystem::path(sessionMediaDirText);
    AveMediaBridgeImportOptions options{};
    options.structSize = importOptionsSizeBeforeWaveformCallback();
    options.inputPath = inputPath.c_str();
    options.sessionMediaDir = sessionMediaDir.c_str();
    options.onProgress = printProgressSmokeCallback;
    options.shouldCancel = nullptr;
    options.userData = &progressState;

    const int result = importSessionEx(&options);
    if (result != 0) {
        std::cerr << "Result: FAIL\n";
        std::cerr << "AveMediaBridge_ImportAudioToSessionEx returned " << result << "\n";
        printDllLastErrorText(module);
        FreeLibrary(module);
        return result;
    }

    FreeLibrary(module);
    const std::filesystem::path sessionPath(sessionMediaDirText);
    SessionArtifactSummary summary;
    std::string summaryError;
    if (!readSessionArtifactSummary(sessionPath, summary, summaryError)) {
        std::cerr << "ERROR: " << summaryError << ".\n";
        return 1;
    }

    std::cout << "Result: OK\n";
    std::cout << "Progress callbacks observed: " << progressState.callbackCount << "\n";
    std::cout << "Progress frames monotonic: " << (progressState.monotonic ? "yes" : "no") << "\n";
    printSessionArtifactSummary(sessionMediaDirText, summary);

    const bool finalProgressMatches =
        progressState.callbackCount > 0 &&
        progressState.lastFramesWritten == static_cast<std::uint64_t>(summary.frames);
    std::cout << "Final progress frames match audio_info.json: " << (finalProgressMatches ? "yes" : "no") << "\n";
    return progressState.callbackCount > 0 &&
            progressState.monotonic &&
            summary.metadataExists &&
            summary.audioInfoExists &&
            summary.audioDataExists &&
            summary.sizeMatch &&
            finalProgressMatches
        ? 0
        : 1;
}

int runSmokeDllImportSessionLiveReadable(const std::string& inputPathText, const std::string& sessionMediaDirText) {
    const std::filesystem::path exeDir = getExecutableDirectory();
    const std::filesystem::path ffmpegDir = exeDir / "Lib" / "ffmpeg";
    const std::filesystem::path dllPath = exeDir / "AveMediaBridge.dll";

    std::wcout << L"Smoke DLL import session live-readable\n";
    std::wcout << L"  DLL: " << dllPath.wstring() << L"\n";
    std::wcout << L"  FFmpeg DLL dir: " << ffmpegDir.wstring() << L"\n";

    HMODULE module = loadBridgeDll(exeDir, ffmpegDir);
    if (!module) {
        std::wcerr << L"ERROR: LoadLibraryW(AveMediaBridge.dll) failed. Win32 error: " << GetLastError() << L"\n";
        return 1;
    }

    auto importSessionEx =
        reinterpret_cast<ImportAudioToSessionExFn>(GetProcAddress(module, "AveMediaBridge_ImportAudioToSessionEx"));
    if (!importSessionEx) {
        std::cerr << "ERROR: GetProcAddress(AveMediaBridge_ImportAudioToSessionEx) failed. Win32 error: " << GetLastError() << "\n";
        FreeLibrary(module);
        return 1;
    }

    const std::wstring inputPath = stringToWide(inputPathText);
    const std::wstring sessionMediaDir = stringToWide(sessionMediaDirText);
    if (inputPath.empty() || sessionMediaDir.empty()) {
        std::cerr << "ERROR: unable to convert input or session path to UTF-16.\n";
        FreeLibrary(module);
        return 1;
    }

    ProgressSmokeState progressState;
    progressState.liveReadableCheck = true;
    progressState.sessionMediaDir = std::filesystem::path(sessionMediaDirText);

    AveMediaBridgeImportOptions options{};
    options.structSize = importOptionsSizeBeforeWaveformCallback();
    options.inputPath = inputPath.c_str();
    options.sessionMediaDir = sessionMediaDir.c_str();
    options.onProgress = printProgressSmokeCallback;
    options.shouldCancel = nullptr;
    options.userData = &progressState;

    const int result = importSessionEx(&options);
    if (result != 0) {
        std::cerr << "Result: FAIL\n";
        std::cerr << "AveMediaBridge_ImportAudioToSessionEx returned " << result << "\n";
        printDllLastErrorText(module);
        FreeLibrary(module);
        return result;
    }

    FreeLibrary(module);
    const std::filesystem::path sessionPath(sessionMediaDirText);
    SessionArtifactSummary summary;
    std::string summaryError;
    if (!readSessionArtifactSummary(sessionPath, summary, summaryError)) {
        std::cerr << "ERROR: " << summaryError << ".\n";
        return 1;
    }

    const bool finalProgressMatches =
        progressState.callbackCount > 0 &&
        progressState.lastFramesWritten == static_cast<std::uint64_t>(summary.frames);

    std::cout << "Result: OK\n";
    std::cout << "Progress callbacks observed: " << progressState.callbackCount << "\n";
    std::cout << "Progress frames monotonic: " << (progressState.monotonic ? "yes" : "no") << "\n";
    std::cout << "First positive progress checked: " << (progressState.firstPositiveProgressChecked ? "yes" : "no") << "\n";
    std::cout << "original_f32.bin existed at first positive progress: "
              << (progressState.firstPositiveAudioExists ? "yes" : "no") << "\n";
    std::cout << "File size covered first committed bytes: "
              << (progressState.firstPositiveSizeOk ? "yes" : "no") << "\n";
    std::cout << "Temporary json files existed at first positive progress: "
              << (progressState.firstPositiveTempJsonExists ? "yes" : "no") << "\n";
    std::cout << "File size never behind committed bytes: "
              << (progressState.liveFileSizeNeverBehind ? "yes" : "no") << "\n";
    printSessionArtifactSummary(sessionMediaDirText, summary);
    std::cout << "Final progress frames match audio_info.json: " << (finalProgressMatches ? "yes" : "no") << "\n";

    return progressState.callbackCount > 0 &&
            progressState.monotonic &&
            progressState.firstPositiveProgressChecked &&
            progressState.firstPositiveAudioExists &&
            progressState.firstPositiveSizeOk &&
            progressState.firstPositiveTempJsonExists &&
            progressState.liveFileSizeNeverBehind &&
            summary.metadataExists &&
            summary.audioInfoExists &&
            summary.audioDataExists &&
            summary.sizeMatch &&
            finalProgressMatches
        ? 0
        : 1;
}

int runSmokeDllImportSessionWaveform(const std::string& inputPathText, const std::string& sessionMediaDirText) {
    const std::filesystem::path exeDir = getExecutableDirectory();
    const std::filesystem::path ffmpegDir = exeDir / "Lib" / "ffmpeg";
    const std::filesystem::path dllPath = exeDir / "AveMediaBridge.dll";

    std::wcout << L"Smoke DLL import session waveform\n";
    std::wcout << L"  DLL: " << dllPath.wstring() << L"\n";
    std::wcout << L"  FFmpeg DLL dir: " << ffmpegDir.wstring() << L"\n";

    HMODULE module = loadBridgeDll(exeDir, ffmpegDir);
    if (!module) {
        std::wcerr << L"ERROR: LoadLibraryW(AveMediaBridge.dll) failed. Win32 error: " << GetLastError() << L"\n";
        return 1;
    }

    auto importSessionEx =
        reinterpret_cast<ImportAudioToSessionExFn>(GetProcAddress(module, "AveMediaBridge_ImportAudioToSessionEx"));
    if (!importSessionEx) {
        std::cerr << "ERROR: GetProcAddress(AveMediaBridge_ImportAudioToSessionEx) failed. Win32 error: " << GetLastError() << "\n";
        FreeLibrary(module);
        return 1;
    }

    const std::wstring inputPath = stringToWide(inputPathText);
    const std::wstring sessionMediaDir = stringToWide(sessionMediaDirText);
    if (inputPath.empty() || sessionMediaDir.empty()) {
        std::cerr << "ERROR: unable to convert input or session path to UTF-16.\n";
        FreeLibrary(module);
        return 1;
    }

    WaveformChunkSmokeState waveformState;
    AveMediaBridgeImportOptions options{};
    options.structSize = sizeof(options);
    options.inputPath = inputPath.c_str();
    options.sessionMediaDir = sessionMediaDir.c_str();
    options.onProgress = waveformProgressSmokeCallback;
    options.shouldCancel = nullptr;
    options.userData = &waveformState;
    options.onWaveformChunk = waveformChunkSmokeCallback;

    const int result = importSessionEx(&options);
    if (result != 0) {
        std::cerr << "Result: FAIL\n";
        std::cerr << "AveMediaBridge_ImportAudioToSessionEx returned " << result << "\n";
        printDllLastErrorText(module);
        FreeLibrary(module);
        return result;
    }

    FreeLibrary(module);
    const std::filesystem::path sessionPath(sessionMediaDirText);
    SessionArtifactSummary summary;
    std::string summaryError;
    if (!readSessionArtifactSummary(sessionPath, summary, summaryError)) {
        std::cerr << "ERROR: " << summaryError << ".\n";
        return 1;
    }

    std::cout << "Result: OK\n";
    std::cout << "Waveform chunk count: " << waveformState.waveformChunkCount << "\n";
    std::cout << "Waveform firstFrame first: " << waveformState.firstFrameFirst << "\n";
    std::cout << "Waveform firstFrame last: " << waveformState.firstFrameLast << "\n";
    std::cout << "Waveform total bins received: " << waveformState.totalBinsReceived << "\n";
    std::cout << "Waveform firstFrame monotonic: " << (waveformState.firstFrameMonotonic ? "yes" : "no") << "\n";
    std::cout << "Waveform binCount valid: " << (waveformState.binCountValid ? "yes" : "no") << "\n";
    std::cout << "Waveform valuesPerBin valid: " << (waveformState.valuesPerBinValid ? "yes" : "no") << "\n";
    std::cout << "Waveform min/max values finite: " << (waveformState.minMaxPairsValid ? "yes" : "no") << "\n";
    std::cout << "Waveform long-form energy observed: " << (waveformState.longFormEnergyObserved ? "yes" : "no") << "\n";
    std::cout << "Waveform long-form energy valid: " << (waveformState.longFormEnergyValid ? "yes" : "no") << "\n";
    std::cout << "Waveform long-form energy frameCount total: " << waveformState.energyFrameCountTotal << "\n";
    std::cout << "Progress callbacks observed: " << waveformState.progressCallbackCount << "\n";
    std::cout << "Progress frames monotonic: " << (waveformState.progressMonotonic ? "yes" : "no") << "\n";
    printSessionArtifactSummary(sessionMediaDirText, summary);
    std::cout << "Final import byte match: " << (summary.sizeMatch ? "yes" : "no") << "\n";

    return waveformState.waveformChunkCount > 0 &&
            waveformState.firstFrameMonotonic &&
            waveformState.binCountValid &&
            waveformState.valuesPerBinValid &&
            waveformState.minMaxPairsValid &&
            waveformState.longFormEnergyObserved &&
            waveformState.longFormEnergyValid &&
            waveformState.energyFrameCountTotal > 0 &&
            waveformState.totalBinsReceived > 0 &&
            waveformState.progressCallbackCount > 0 &&
            waveformState.progressMonotonic &&
            summary.metadataExists &&
            summary.audioInfoExists &&
            summary.audioDataExists &&
            summary.sizeMatch
        ? 0
        : 1;
}

int runWaveformFullScalePrepostSwrTest(const std::string& inputPathText) {
    std::filesystem::path sessionDir;
    std::string sessionError;
    if (!createFullScaleDiagSessionDir(sessionDir, sessionError)) {
        std::cerr << "ERROR: " << sessionError << "\n";
        return 1;
    }

    const std::filesystem::path exeDir = getExecutableDirectory();
    const std::filesystem::path ffmpegDir = exeDir / "Lib" / "ffmpeg";
    const std::filesystem::path dllPath = exeDir / "AveMediaBridge.dll";

    std::wcout << L"Waveform full-scale pre/post SWR diagnostic\n";
    std::wcout << L"  DLL: " << dllPath.wstring() << L"\n";
    std::wcout << L"  FFmpeg DLL dir: " << ffmpegDir.wstring() << L"\n";
    std::wcout << L"  Session media dir: " << sessionDir.wstring() << L"\n";

    HMODULE module = loadBridgeDll(exeDir, ffmpegDir);
    if (!module) {
        std::wcerr << L"ERROR: LoadLibraryW(AveMediaBridge.dll) failed. Win32 error: " << GetLastError() << L"\n";
        return 1;
    }

    auto importSessionEx =
        reinterpret_cast<ImportAudioToSessionExFn>(GetProcAddress(module, "AveMediaBridge_ImportAudioToSessionEx"));
    if (!importSessionEx) {
        std::cerr << "ERROR: GetProcAddress(AveMediaBridge_ImportAudioToSessionEx) failed. Win32 error: " << GetLastError() << "\n";
        FreeLibrary(module);
        return 1;
    }

    const std::wstring inputPath = stringToWide(inputPathText);
    const std::wstring sessionMediaDir = sessionDir.wstring();
    if (inputPath.empty() || sessionMediaDir.empty()) {
        std::cerr << "ERROR: unable to convert input or session path to UTF-16.\n";
        FreeLibrary(module);
        return 1;
    }

    ScopedEnvironmentVariable fullScaleDiagEnv(L"AVEMEDIABRIDGE_FULL_SCALE_CLIP_DIAG", L"1");
    if (!fullScaleDiagEnv.ok()) {
        std::cerr << "ERROR: unable to enable AVEMEDIABRIDGE_FULL_SCALE_CLIP_DIAG. Win32 error: "
                  << GetLastError()
                  << "\n";
        FreeLibrary(module);
        return 1;
    }

    AveMediaBridgeImportOptions options{};
    options.structSize = importOptionsSizeBeforeWaveformCallback();
    options.inputPath = inputPath.c_str();
    options.sessionMediaDir = sessionMediaDir.c_str();

    const int result = importSessionEx(&options);
    if (result != 0) {
        std::cerr << "Result: FAIL\n";
        std::cerr << "AveMediaBridge_ImportAudioToSessionEx returned " << result << "\n";
        printDllLastErrorText(module);
        FreeLibrary(module);
        return result;
    }

    FreeLibrary(module);
    SessionArtifactSummary summary;
    std::string summaryError;
    if (!readSessionArtifactSummary(sessionDir, summary, summaryError)) {
        std::cerr << "ERROR: " << summaryError << ".\n";
        return 1;
    }

    std::cout << "Result: OK\n";
    printSessionArtifactSummary(sessionDir.string(), summary);
    std::cout << "Full-scale diagnostic completed: yes\n";
    return summary.metadataExists && summary.audioInfoExists && summary.audioDataExists && summary.sizeMatch ? 0 : 1;
}

int runSmokeDllImportSessionCancel(const std::string& inputPathText, const std::string& sessionMediaDirText) {
    const std::filesystem::path exeDir = getExecutableDirectory();
    const std::filesystem::path ffmpegDir = exeDir / "Lib" / "ffmpeg";
    const std::filesystem::path dllPath = exeDir / "AveMediaBridge.dll";

    std::wcout << L"Smoke DLL import session cancel\n";
    std::wcout << L"  DLL: " << dllPath.wstring() << L"\n";
    std::wcout << L"  FFmpeg DLL dir: " << ffmpegDir.wstring() << L"\n";

    HMODULE module = loadBridgeDll(exeDir, ffmpegDir);
    if (!module) {
        std::wcerr << L"ERROR: LoadLibraryW(AveMediaBridge.dll) failed. Win32 error: " << GetLastError() << L"\n";
        return 1;
    }

    auto importSessionEx =
        reinterpret_cast<ImportAudioToSessionExFn>(GetProcAddress(module, "AveMediaBridge_ImportAudioToSessionEx"));
    if (!importSessionEx) {
        std::cerr << "ERROR: GetProcAddress(AveMediaBridge_ImportAudioToSessionEx) failed. Win32 error: " << GetLastError() << "\n";
        FreeLibrary(module);
        return 1;
    }

    const std::wstring inputPath = stringToWide(inputPathText);
    const std::wstring sessionMediaDir = stringToWide(sessionMediaDirText);
    if (inputPath.empty() || sessionMediaDir.empty()) {
        std::cerr << "ERROR: unable to convert input or session path to UTF-16.\n";
        FreeLibrary(module);
        return 1;
    }

    ProgressSmokeState progressState;
    progressState.cancelAfterFirstProgress = true;
    progressState.sessionMediaDir = std::filesystem::path(sessionMediaDirText);

    AveMediaBridgeImportOptions options{};
    options.structSize = importOptionsSizeBeforeWaveformCallback();
    options.inputPath = inputPath.c_str();
    options.sessionMediaDir = sessionMediaDir.c_str();
    options.onProgress = printProgressSmokeCallback;
    options.shouldCancel = cancelSmokeCallback;
    options.userData = &progressState;

    const int result = importSessionEx(&options);
    const std::wstring lastError = getDllLastErrorText(module);
    FreeLibrary(module);

    const std::filesystem::path sessionPath(sessionMediaDirText);
    SessionArtifactSummary summary;
    std::string summaryError;
    if (!readSessionArtifactSummary(sessionPath, summary, summaryError)) {
        std::cerr << "ERROR: " << summaryError << ".\n";
        return 1;
    }

    const bool noFinalPair = !(summary.audioInfoExists && summary.audioDataExists);
    const bool audioDataRemoved = !summary.audioDataExists;
    const bool noTempJsonFiles = !sessionTempJsonFilesExist(sessionPath);
    const bool canceledText = lastError.find(L"canceled") != std::wstring::npos;

    std::cout << "Import return code: " << result << "\n";
    std::wcout << L"DLL last error: " << lastError << L"\n";
    std::cout << "Progress callbacks observed: " << progressState.callbackCount << "\n";
    std::cout << "Progress frames monotonic: " << (progressState.monotonic ? "yes" : "no") << "\n";
    std::cout << "No final audio_info/original_f32 pair remains: " << (noFinalPair ? "yes" : "no") << "\n";
    std::cout << "original_f32.bin removed: " << (audioDataRemoved ? "yes" : "no") << "\n";
    std::cout << "Temporary json files removed: " << (noTempJsonFiles ? "yes" : "no") << "\n";

    if (result == AVEMEDIABRIDGE_IMPORT_RESULT_CANCELED &&
        canceledText &&
        noFinalPair &&
        audioDataRemoved &&
        noTempJsonFiles) {
        std::cout << "Result: OK\n";
        return 0;
    }

    std::cout << "Result: FAIL\n";
    return 1;
}

std::vector<std::filesystem::path> collectMediaFiles(const std::filesystem::path& folder, bool recursive) {
    std::vector<std::filesystem::path> files;
    if (!std::filesystem::exists(folder)) {
        return files;
    }

    if (recursive) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(folder)) {
            if (entry.is_regular_file() && hasCommonMediaExtension(entry.path())) {
                files.push_back(entry.path());
            }
        }
    } else {
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            if (entry.is_regular_file() && hasCommonMediaExtension(entry.path())) {
                files.push_back(entry.path());
            }
        }
    }

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return lowerString(a.string()) < lowerString(b.string());
    });
    return files;
}

void importOneFromPrompt() {
    std::cout << "Media file path: ";
    std::string path;
    std::getline(std::cin, path);
    path = stripMatchingQuotes(path);
    if (path.empty()) {
        std::cout << "No path entered.\n";
        return;
    }
    importOneFile(path, true);
}

void importFolderFromPrompt() {
    std::cout << "Folder path: ";
    std::string folderText;
    std::getline(std::cin, folderText);
    folderText = stripMatchingQuotes(folderText);
    if (folderText.empty()) {
        std::cout << "No folder entered.\n";
        return;
    }

    std::cout << "Recursive scan? [y/N]: ";
    std::string recursiveText;
    std::getline(std::cin, recursiveText);
    const bool recursive = !recursiveText.empty() && (recursiveText[0] == 'y' || recursiveText[0] == 'Y');

    const std::vector<std::filesystem::path> files = collectMediaFiles(folderText, recursive);
    if (files.empty()) {
        std::cout << "No common media files found.\n";
        return;
    }

    AveMediaBridge::AveMediaBridge bridge;
    int ok = 0;
    int fail = 0;
    int noAudio = 0;

    for (const auto& file : files) {
        std::cout << "Importing: " << file.string() << "\n";
        AudioImportResult result = bridge.importAudio(file.string());
        if (result.hasUsableAudio()) {
            ++ok;
            std::cout << "  OK"
                      << " frames=" << result.audio.frameCount()
                      << " rate=" << result.audio.sampleRate
                      << " channels=" << result.audio.channels
                      << "\n";
        } else {
            ++fail;
            if (result.error.find("no audio stream") != std::string::npos) {
                ++noAudio;
            }
            std::cout << "  FAIL: " << result.error << "\n";
        }
    }

    std::cout << "\nSummary\n";
    std::cout << "  Checked: " << files.size() << "\n";
    std::cout << "  OK: " << ok << "\n";
    std::cout << "  FAIL: " << fail << "\n";
    std::cout << "  NO AUDIO: " << noAudio << "\n";
}

void runProcessorTests() {
    std::cout << "Media file path: ";
    std::string path;
    std::getline(std::cin, path);
    path = stripMatchingQuotes(path);
    if (path.empty()) {
        std::cout << "No path entered.\n";
        return;
    }

    AveMediaBridge::AveMediaBridge bridge;
    AudioImportResult result = bridge.importAudio(path);
    if (!result.hasUsableAudio()) {
        printImportReport(result);
        return;
    }

    std::cout << "Imported AudioBufferF32: "
              << result.audio.frameCount() << " frames, "
              << result.audio.channels << " channels, "
              << result.audio.sampleRate << " Hz\n";
    printStats("Before:", result.stats);

    AudioBufferF32 gainBuffer = result.audio;
    AudioBufferProcessor::applyGain(gainBuffer, 0.5f);
    printStats("After gain 0.5:", AveMediaBridge::computeAudioStats(gainBuffer));

    AudioBufferF32 normalized = result.audio;
    AudioBufferProcessor::normalizePeak(normalized, 0.9f);
    printStats("After normalizePeak 0.9:", AveMediaBridge::computeAudioStats(normalized));

    if (result.audio.channels > 1) {
        AudioBufferF32 mono;
        if (AudioBufferProcessor::makeMonoAverage(result.audio, mono)) {
            printStats("After mono average:", AveMediaBridge::computeAudioStats(mono));
            std::cout << "Mono frames: " << mono.frameCount() << "\n";
        } else {
            std::cout << "Mono average failed.\n";
        }
    } else {
        std::cout << "Mono average skipped: input is already mono.\n";
    }
}

void runMenu() {
    while (true) {
        std::cout << "\n============================================================\n";
        std::cout << "AveMediaBridge Lab\n";
        std::cout << "============================================================\n\n";
        std::cout << "1. Import one media file\n";
        std::cout << "2. Import all files from folder\n";
        std::cout << "3. Run AudioBufferF32 processor tests on one file\n";
        std::cout << "4. Show FFmpeg library versions\n";
        std::cout << "5. Show project paths\n";
        std::cout << "0. Exit\n\n";
        std::cout << "Select option: ";

        std::string choice;
        std::getline(std::cin, choice);
        choice = trim(choice);

        if (choice == "1") {
            importOneFromPrompt();
            waitForEnter();
        } else if (choice == "2") {
            importFolderFromPrompt();
            waitForEnter();
        } else if (choice == "3") {
            runProcessorTests();
            waitForEnter();
        } else if (choice == "4") {
            printFfmpegVersions();
            waitForEnter();
        } else if (choice == "5") {
            printProjectPaths();
            waitForEnter();
        } else if (choice == "0") {
            return;
        } else {
            std::cout << "Unknown option: " << choice << "\n";
            waitForEnter();
        }
    }
}

std::string joinArgs(int argc, char** argv, int first) {
    std::string joined;
    for (int i = first; i < argc; ++i) {
        if (!joined.empty()) {
            joined += ' ';
        }
        joined += argv[i];
    }
    return stripMatchingQuotes(joined);
}

}  // namespace

int main(int argc, char** argv) {
    av_log_set_level(AV_LOG_QUIET);

    if (argc <= 1) {
        runMenu();
        return 0;
    }

    const std::string firstArg = argv[1];
    if (firstArg == "--versions") {
        printFfmpegVersions();
        return 0;
    }
    if (firstArg == "--paths") {
        printProjectPaths();
        return 0;
    }
    if (firstArg == "smoke-dll-transform") {
        if (argc < 4) {
            std::cout << "Usage: AveMediaBridgeLabApp.exe smoke-dll-transform <input-file> <output-wav>\n";
            return 1;
        }
        const std::string inputPath = stripMatchingQuotes(argv[2]);
        const std::string outputPath = stripMatchingQuotes(joinArgs(argc, argv, 3));
        return runSmokeDllTransform(inputPath, outputPath);
    }
    if (firstArg == "smoke-dll-probe") {
        if (argc < 4) {
            std::cout << "Usage: AveMediaBridgeLabApp.exe smoke-dll-probe <input-file> <output-json>\n";
            return 1;
        }
        const std::string inputPath = stripMatchingQuotes(argv[2]);
        const std::string outputPath = stripMatchingQuotes(joinArgs(argc, argv, 3));
        return runSmokeDllProbe(inputPath, outputPath);
    }
    if (firstArg == "smoke-dll-import-session") {
        if (argc < 4) {
            std::cout << "Usage: AveMediaBridgeLabApp.exe smoke-dll-import-session <input-file> <session-media-dir>\n";
            return 1;
        }
        const std::string inputPath = stripMatchingQuotes(argv[2]);
        const std::string sessionMediaDir = stripMatchingQuotes(joinArgs(argc, argv, 3));
        return runSmokeDllImportSession(inputPath, sessionMediaDir);
    }
    if (firstArg == "smoke-dll-import-session-progress") {
        if (argc < 4) {
            std::cout << "Usage: AveMediaBridgeLabApp.exe smoke-dll-import-session-progress <input-file> <session-media-dir>\n";
            return 1;
        }
        const std::string inputPath = stripMatchingQuotes(argv[2]);
        const std::string sessionMediaDir = stripMatchingQuotes(joinArgs(argc, argv, 3));
        return runSmokeDllImportSessionProgress(inputPath, sessionMediaDir);
    }
    if (firstArg == "smoke-dll-import-session-live-readable") {
        if (argc < 4) {
            std::cout << "Usage: AveMediaBridgeLabApp.exe smoke-dll-import-session-live-readable <input-file> <session-media-dir>\n";
            return 1;
        }
        const std::string inputPath = stripMatchingQuotes(argv[2]);
        const std::string sessionMediaDir = stripMatchingQuotes(joinArgs(argc, argv, 3));
        return runSmokeDllImportSessionLiveReadable(inputPath, sessionMediaDir);
    }
    if (firstArg == "smoke-dll-import-session-waveform") {
        if (argc < 4) {
            std::cout << "Usage: AveMediaBridgeLabApp.exe smoke-dll-import-session-waveform <input-file> <session-media-dir>\n";
            return 1;
        }
        const std::string inputPath = stripMatchingQuotes(argv[2]);
        const std::string sessionMediaDir = stripMatchingQuotes(joinArgs(argc, argv, 3));
        return runSmokeDllImportSessionWaveform(inputPath, sessionMediaDir);
    }
    if (firstArg == "waveform-energy-chunk-contract-test") {
        return runWaveformEnergyChunkContractTest();
    }
    if (firstArg == "full-scale-sample-stats-contract-test") {
        return runFullScaleSampleStatsContractTest();
    }
    if (firstArg == "waveform-full-scale-prepost-swr-test") {
        if (argc < 3) {
            std::cout << "Usage: AveMediaBridgeLabApp.exe waveform-full-scale-prepost-swr-test <input-file>\n";
            return 1;
        }
        const std::string inputPath = stripMatchingQuotes(joinArgs(argc, argv, 2));
        return runWaveformFullScalePrepostSwrTest(inputPath);
    }
    if (firstArg == "smoke-dll-import-session-cancel") {
        if (argc < 4) {
            std::cout << "Usage: AveMediaBridgeLabApp.exe smoke-dll-import-session-cancel <input-file> <session-media-dir>\n";
            return 1;
        }
        const std::string inputPath = stripMatchingQuotes(argv[2]);
        const std::string sessionMediaDir = stripMatchingQuotes(joinArgs(argc, argv, 3));
        return runSmokeDllImportSessionCancel(inputPath, sessionMediaDir);
    }

    const std::string inputPath = joinArgs(argc, argv, 1);
    return importOneFile(inputPath, true);
}
