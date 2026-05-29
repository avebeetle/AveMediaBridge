#include "AveMediaBridge/AveMediaBridgeApi.hpp"

#include "AveMediaBridge/AveMediaBridge.hpp"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr const wchar_t* kVersionString = L"AveMediaBridge 0.1.0 API v1";
thread_local std::wstring g_lastError;

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

std::int64_t decodedFrames(const AveMediaBridge::AudioImportResult& result) {
    return result.audio.isConsistent() ? result.audio.frameCount() : 0;
}

double durationSeconds(const AveMediaBridge::AudioImportResult& result) {
    if (result.source.durationSeconds > 0.0) {
        return result.source.durationSeconds;
    }
    return result.audio.durationSeconds();
}

int effectiveSampleRate(const AveMediaBridge::AudioImportResult& result) {
    if (result.audio.sampleRate > 0) {
        return result.audio.sampleRate;
    }
    if (result.decode.outputSampleRate > 0) {
        return result.decode.outputSampleRate;
    }
    return result.selectedAudio.sampleRate;
}

int effectiveChannels(const AveMediaBridge::AudioImportResult& result) {
    if (result.audio.channels > 0) {
        return result.audio.channels;
    }
    if (result.decode.outputChannels > 0) {
        return result.decode.outputChannels;
    }
    return result.selectedAudio.channels;
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

bool writeProbeJson(
    const std::filesystem::path& outputPath,
    const std::string& sourcePathUtf8,
    const AveMediaBridge::AudioImportResult& result,
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
    json << "  \"sourcePath\": " << jsonString(sourcePathUtf8) << ",\n";
    json << "  \"hasAudio\": " << (hasAudio(result) ? "true" : "false") << ",\n";
    json << "  \"selectedAudioStreamIndex\": " << result.selectedAudio.index << ",\n";
    json << "  \"sampleRate\": " << effectiveSampleRate(result) << ",\n";
    json << "  \"channels\": " << effectiveChannels(result) << ",\n";
    json << "  \"frames\": " << decodedFrames(result) << ",\n";
    json << "  \"durationSec\": " << durationSeconds(result) << ",\n";
    json << "  \"formatName\": " << jsonString(result.source.formatName) << ",\n";
    json << "  \"formatLongName\": " << jsonString(result.source.formatLongName) << ",\n";
    json << "  \"codecName\": " << jsonString(result.selectedAudio.codecName) << ",\n";
    json << "  \"codecId\": " << result.selectedAudio.codecId << ",\n";
    json << "  \"streamCount\": " << result.source.streamCount << ",\n";
    json << "  \"audioStreamCount\": " << result.probe.audioStreamCount << ",\n";
    json << "  \"probeMode\": \"decode_v1\",\n";
    json << "  \"selectedAudioStream\": ";
    writeSelectedAudioJson(json, result.selectedAudio, "  ");
    json << ",\n";
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

        AveMediaBridge::AveMediaBridge bridge;
        AveMediaBridge::AudioImportResult imported = bridge.importAudio(input);
        if (!writeProbeJson(outputFsPath, input, imported, error)) {
            setLastErrorText(error);
            return 3;
        }

        if (!imported.error.empty() && !imported.probe.streamInfoFound) {
            setLastErrorText(imported.error);
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
