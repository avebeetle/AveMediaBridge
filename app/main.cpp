#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#define WIN32_LEAN_AND_MEAN

#include "AveMediaBridge/AveMediaBridge.hpp"
#include "AveMediaBridge/Core/AudioStats.hpp"
#include "AveMediaBridge/Process/AudioBufferProcessor.hpp"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
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

void configureDllSearchPath(const std::filesystem::path& exeDir, const std::filesystem::path& ffmpegDir) {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    AddDllDirectory(exeDir.c_str());
    AddDllDirectory(ffmpegDir.c_str());
}

HMODULE loadBridgeDll(const std::filesystem::path& exeDir, const std::filesystem::path& ffmpegDir) {
    configureDllSearchPath(exeDir, ffmpegDir);
    return LoadLibraryW(L"AveMediaBridge.dll");
}

void printDllLastErrorText(HMODULE module) {
    if (!module) {
        return;
    }

    auto getLastErrorText = reinterpret_cast<GetLastErrorTextFn>(GetProcAddress(module, "AveMediaBridge_GetLastErrorText"));
    if (!getLastErrorText) {
        return;
    }

    wchar_t buffer[2048] = {};
    const int result = getLastErrorText(buffer, static_cast<int>(sizeof(buffer) / sizeof(buffer[0])));
    if ((result == 0 || result == 2) && buffer[0] != L'\0') {
        std::wcerr << L"DLL last error: " << buffer << L"\n";
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
    const std::filesystem::path metadataPath = sessionPath / "metadata.json";
    const std::filesystem::path audioInfoPath = sessionPath / "audio_info.json";
    const std::filesystem::path audioDataPath = sessionPath / "original_f32.bin";

    std::string audioInfoText;
    std::int64_t sampleRate = 0;
    std::int64_t channels = 0;
    std::int64_t frames = 0;
    if (!readTextFile(audioInfoPath, audioInfoText) ||
        !extractJsonInt64(audioInfoText, "sampleRate", sampleRate) ||
        !extractJsonInt64(audioInfoText, "channels", channels) ||
        !extractJsonInt64(audioInfoText, "frames", frames)) {
        std::cerr << "ERROR: unable to read generated audio_info.json summary.\n";
        return 1;
    }

    std::error_code fsError;
    const bool metadataExists = std::filesystem::exists(metadataPath, fsError);
    const bool audioInfoExists = std::filesystem::exists(audioInfoPath, fsError);
    const bool audioDataExists = std::filesystem::exists(audioDataPath, fsError);
    const auto actualBytes = audioDataExists ? std::filesystem::file_size(audioDataPath, fsError) : 0;
    if (fsError) {
        std::cerr << "ERROR: unable to inspect generated artifacts: " << fsError.message() << "\n";
        return 1;
    }

    const std::int64_t expectedBytes = frames * channels * static_cast<std::int64_t>(sizeof(float));
    const bool sizeMatch = audioDataExists && actualBytes == static_cast<std::uintmax_t>(expectedBytes);

    std::cout << "Result: OK\n";
    std::cout << "Session media dir: " << sessionMediaDirText << "\n";
    std::cout << "Streaming import: yes\n";
    std::cout << "Sample rate: " << sampleRate << "\n";
    std::cout << "Channels: " << channels << "\n";
    std::cout << "Frames: " << frames << "\n";
    std::cout << "Expected bytes: " << expectedBytes << "\n";
    std::cout << "Actual original_f32.bin bytes: " << actualBytes << "\n";
    std::cout << "Size match: " << (sizeMatch ? "yes" : "no") << "\n";
    std::cout << "Generated:\n";
    std::cout << "  metadata.json " << (metadataExists ? "yes" : "missing") << "\n";
    std::cout << "  audio_info.json " << (audioInfoExists ? "yes" : "missing") << "\n";
    std::cout << "  original_f32.bin " << (audioDataExists ? "yes" : "missing") << "\n";
    return metadataExists && audioInfoExists && audioDataExists && sizeMatch ? 0 : 1;
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

    const std::string inputPath = joinArgs(argc, argv, 1);
    return importOneFile(inputPath, true);
}
