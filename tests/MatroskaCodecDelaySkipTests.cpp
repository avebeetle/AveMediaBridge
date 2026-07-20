#include "include/AveMediaBridge/AveMediaBridgeApi.hpp"
#include "src/Ffmpeg/FfmpegDeleters.hpp"
#include "src/Probe/FrameCountPolicy.hpp"
#include "src/Probe/PacketScan.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace {

namespace Ffmpeg = AveMediaBridge::Ffmpeg;
namespace Probe = AveMediaBridge::Probe;

struct WaveInfo {
    std::uint64_t dataOffset = 0;
    std::uint64_t dataBytes = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
    std::uint16_t formatTag = 0;
    std::uint16_t bitsPerSample = 0;
};

class ScopedTempDirectory {
public:
    ScopedTempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            (L"avemediabridge_matroska_codec_delay_" +
             std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(stamp));
        std::filesystem::create_directories(path_);
    }

    ~ScopedTempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::uint16_t readLe16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>(bytes[0]) |
        static_cast<std::uint16_t>(bytes[1] << 8);
}

std::uint32_t readLe32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
}

WaveInfo readWaveInfo(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open PCM master");
    }

    std::uint8_t header[12]{};
    input.read(reinterpret_cast<char*>(header), sizeof(header));
    if (input.gcount() != sizeof(header) ||
        std::string(reinterpret_cast<char*>(header), 4) != "RIFF" ||
        std::string(reinterpret_cast<char*>(header + 8), 4) != "WAVE") {
        throw std::runtime_error("PCM master is not RIFF/WAVE");
    }

    WaveInfo info;
    bool foundFormat = false;
    bool foundData = false;
    while (input && (!foundFormat || !foundData)) {
        std::uint8_t chunkHeader[8]{};
        input.read(reinterpret_cast<char*>(chunkHeader), sizeof(chunkHeader));
        if (input.gcount() != sizeof(chunkHeader)) {
            break;
        }
        const std::string id(reinterpret_cast<char*>(chunkHeader), 4);
        const std::uint32_t size = readLe32(chunkHeader + 4);
        const std::uint64_t payloadOffset = static_cast<std::uint64_t>(input.tellg());
        if (id == "fmt " && size >= 16) {
            std::uint8_t format[16]{};
            input.read(reinterpret_cast<char*>(format), sizeof(format));
            info.formatTag = readLe16(format);
            info.channels = readLe16(format + 2);
            info.sampleRate = readLe32(format + 4);
            info.bitsPerSample = readLe16(format + 14);
            foundFormat = true;
        } else if (id == "data") {
            info.dataOffset = payloadOffset;
            info.dataBytes = size;
            foundData = true;
        }
        input.seekg(static_cast<std::streamoff>(payloadOffset + size + (size & 1U)));
    }

    if (!foundFormat || !foundData || info.formatTag != 3 || info.bitsPerSample != 32) {
        throw std::runtime_error("PCM master must be float32 RIFF/WAVE");
    }
    return info;
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to read audio_info.json");
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::optional<double> jsonNumber(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    const std::size_t keyAt = json.find(token);
    if (keyAt == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t colon = json.find(':', keyAt + token.size());
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    std::size_t consumed = 0;
    try {
        return std::stod(json.substr(colon + 1), &consumed);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> jsonString(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    const std::size_t keyAt = json.find(token);
    if (keyAt == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t colon = json.find(':', keyAt + token.size());
    const std::size_t quote = json.find('"', colon == std::string::npos ? keyAt : colon + 1);
    if (colon == std::string::npos || quote == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t end = json.find('"', quote + 1);
    if (end == std::string::npos) {
        return std::nullopt;
    }
    return json.substr(quote + 1, end - quote - 1);
}

std::vector<float> readFloatFrames(
    const std::filesystem::path& path,
    std::uint64_t dataOffset,
    std::uint64_t firstFrame,
    std::size_t frameCount,
    int channels) {
    const std::uint64_t sampleOffset = firstFrame * static_cast<std::uint64_t>(channels);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open PCM data");
    }
    input.seekg(static_cast<std::streamoff>(dataOffset + sampleOffset * sizeof(float)));
    std::vector<float> values(frameCount * static_cast<std::size_t>(channels));
    input.read(
        reinterpret_cast<char*>(values.data()),
        static_cast<std::streamsize>(values.size() * sizeof(float)));
    if (input.gcount() != static_cast<std::streamsize>(values.size() * sizeof(float))) {
        throw std::runtime_error("PCM data window is incomplete");
    }
    return values;
}

double normalizedCorrelation(const std::vector<float>& left, const std::vector<float>& right) {
    if (left.size() != right.size() || left.empty()) {
        return 0.0;
    }
    long double leftMean = 0.0L;
    long double rightMean = 0.0L;
    for (std::size_t i = 0; i < left.size(); ++i) {
        leftMean += left[i];
        rightMean += right[i];
    }
    leftMean /= left.size();
    rightMean /= right.size();

    long double dot = 0.0L;
    long double leftEnergy = 0.0L;
    long double rightEnergy = 0.0L;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const long double a = left[i] - leftMean;
        const long double b = right[i] - rightMean;
        dot += a * b;
        leftEnergy += a * a;
        rightEnergy += b * b;
    }
    if (leftEnergy <= 0.0L || rightEnergy <= 0.0L) {
        return 0.0;
    }
    return static_cast<double>(dot / std::sqrt(leftEnergy * rightEnergy));
}

double rms(const std::vector<float>& values) {
    long double sum = 0.0L;
    for (float value : values) {
        sum += static_cast<long double>(value) * value;
    }
    return values.empty() ? 0.0 : static_cast<double>(std::sqrt(sum / values.size()));
}

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "matroskaCodecDelaySkipTest: " << message << "\n";
    }
    return condition;
}

struct ProgressCapture {
    std::uint64_t firstEstimatedTotalFrames = 0;
};

void AVEMEDIABRIDGE_CALL captureProgress(
    const AveMediaBridgeImportProgress* progress,
    void* userData) {
    if (!progress || !userData) {
        return;
    }
    auto& capture = *static_cast<ProgressCapture*>(userData);
    if (capture.firstEstimatedTotalFrames == 0 && progress->estimatedTotalFrames > 0) {
        capture.firstEstimatedTotalFrames = progress->estimatedTotalFrames;
    }
}

int run(
    const std::filesystem::path& mediaPath,
    const std::filesystem::path& masterPath,
    std::uint64_t expectedFrames,
    std::int64_t expectedSkip,
    std::uint64_t expectedRawProbeFrames) {
    AVFormatContext* rawFormat = nullptr;
    const std::string mediaUtf8 = mediaPath.u8string();
    int result = avformat_open_input(&rawFormat, mediaUtf8.c_str(), nullptr, nullptr);
    if (result < 0) {
        throw std::runtime_error("failed to open Matroska fixture");
    }
    Ffmpeg::UniqueAVFormatContext format(rawFormat);
    result = avformat_find_stream_info(format.get(), nullptr);
    if (result < 0) {
        throw std::runtime_error("failed to read Matroska stream info");
    }
    const int audioStreamIndex = av_find_best_stream(
        format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioStreamIndex < 0) {
        throw std::runtime_error("Matroska fixture has no selected audio stream");
    }
    AVCodecParameters* codec = format->streams[audioStreamIndex]->codecpar;
    const int sampleRate = codec->sample_rate;
    const int channels = codec->ch_layout.nb_channels;
    const std::int64_t codecDelayFrames = (std::max)(0, codec->initial_padding);
    const AVCodecID codecId = codec->codec_id;
    format.reset();

    constexpr Probe::PacketScanOptions scanOptions{
        4LL * 1024LL * 1024LL,
        3LL * AV_TIME_BASE
    };
    const Probe::AudioPresentationEvidenceScan evidence =
        Probe::scanAudioPresentationEvidence(
            mediaUtf8, audioStreamIndex, sampleRate, codecId, scanOptions);

    bool ok = true;
    ok &= expect(codecDelayFrames == expectedSkip, "unexpected CodecDelay frame count");
    ok &= expect(evidence.packetTiming.warning.empty(), "packet timing scan warning");
    ok &= expect(evidence.gapless.warning.empty(), "gapless scan warning");
    ok &= expect(evidence.gapless.skipSamplesStart == expectedSkip, "CodecDelay was not propagated as start skip");
    ok &= expect(evidence.gapless.skipSamplesEnd == 0, "fixture unexpectedly has terminal discard padding");
    ok &= expect(
        evidence.packetTiming.aacFrameCountCandidateFrames ==
            static_cast<std::int64_t>(expectedFrames) + expectedSkip,
        "physical AAC block count changed");
    ok &= expect(evidence.packetTiming.reachedEof, "packet timing scan did not reach EOF");
    ok &= expect(evidence.gapless.reachedEof, "gapless scan did not reach EOF");
    ok &= expect(evidence.packetTiming.codecFrameCountKnown, "physical codec-frame count is unknown");
    ok &= expect(evidence.packetTiming.codecFrameCountExact, "physical codec-frame count is not exact");
    ok &= expect(
        evidence.packetTiming.codecFrameCountFrames ==
            static_cast<std::int64_t>(expectedFrames) + expectedSkip,
        "generic codec-frame sample count changed");

    const Probe::ExactPacketPresentationEvidence exactEvidence =
        Probe::makeExactPacketPresentationEvidence(evidence);
    const Probe::ExactPacketPresentationBudget exactBudget =
        Probe::resolveExactPacketPresentationBudget(exactEvidence);
    ok &= expect(exactEvidence.initialSkipAuthoritative, "initial skip is not authoritative");
    ok &= expect(exactEvidence.terminalDiscardKnown, "known zero terminal discard was lost");
    ok &= expect(exactEvidence.terminalDiscardFrames == 0, "terminal discard changed");
    ok &= expect(exactBudget.accepted, "exact packet presentation budget was rejected");
    ok &= expect(
        exactBudget.presentationFrames == static_cast<std::int64_t>(expectedFrames),
        "exact packet presentation budget changed");

    ScopedTempDirectory temp;
    const std::filesystem::path mediaDir = temp.path() / L"Media";
    const std::filesystem::path probePath = temp.path() / L"probe.json";
    result = AveMediaBridge_ProbeToJson(mediaPath.c_str(), probePath.c_str());
    ok &= expect(result == 0, "AveMediaBridge probe failed");
    const std::string probeJson = result == 0 ? readText(probePath) : std::string{};
    const auto probeFrames = jsonNumber(probeJson, "decodedSampleFrames");
    const auto rawProbeFrames = jsonNumber(probeJson, "decodedSampleFramesBeforeCorrection");
    const auto probeDuration = jsonNumber(probeJson, "durationSec");
    const auto probeKind = jsonString(probeJson, "decodedSampleFramesKind");
    const auto probeTrust = jsonString(probeJson, "decodedSampleFramesTrust");
    const auto probeSource = jsonString(probeJson, "decodedSampleFramesSource");
    ok &= expect(probeFrames && *probeFrames == expectedFrames, "initial probe presentation frames mismatch");
    ok &= expect(
        rawProbeFrames && *rawProbeFrames == expectedRawProbeFrames,
        "raw rounded container estimate was not preserved");
    ok &= expect(probeDuration && *probeDuration == 2400.021, "container duration metadata changed");
    ok &= expect(probeKind && *probeKind == "exact", "probe authority kind is not exact");
    ok &= expect(probeTrust && *probeTrust == "authoritative", "probe authority trust changed");
    ok &= expect(
        probeSource && *probeSource == "exact_packet_presentation",
        "probe authority source changed");

    ProgressCapture progressCapture;
    AveMediaBridgeImportOptions importOptions{};
    importOptions.structSize = sizeof(importOptions);
    importOptions.inputPath = mediaPath.c_str();
    importOptions.sessionMediaDir = mediaDir.c_str();
    importOptions.onProgress = captureProgress;
    importOptions.userData = &progressCapture;
    result = AveMediaBridge_ImportAudioToSessionEx(&importOptions);
    ok &= expect(result == 0, "AveMediaBridge import failed");
    ok &= expect(
        progressCapture.firstEstimatedTotalFrames == expectedRawProbeFrames,
        "disk preflight estimate unexpectedly changed presentation authority");
    if (!ok) {
        return 1;
    }

    const std::filesystem::path outputPcm = mediaDir / L"original_f32.bin";
    const std::string audioInfo = readText(mediaDir / L"audio_info.json");
    const auto audioInfoFrames = jsonNumber(audioInfo, "frames");
    const auto audioInfoRate = jsonNumber(audioInfo, "sampleRate");
    const auto audioInfoChannels = jsonNumber(audioInfo, "channels");
    ok &= expect(audioInfoFrames && *audioInfoFrames == expectedFrames, "audio_info.frames mismatch");
    ok &= expect(audioInfoRate && *audioInfoRate == sampleRate, "audio_info sample rate mismatch");
    ok &= expect(audioInfoChannels && *audioInfoChannels == channels, "audio_info channel count mismatch");
    const double presentationDuration = static_cast<double>(expectedFrames) / sampleRate;
    ok &= expect(
        std::filesystem::file_size(outputPcm) ==
            expectedFrames * static_cast<std::uint64_t>(channels) * sizeof(float),
        "committed PCM byte size mismatch");

    const WaveInfo master = readWaveInfo(masterPath);
    ok &= expect(master.sampleRate == sampleRate, "master sample rate mismatch");
    ok &= expect(master.channels == channels, "master channel count mismatch");
    ok &= expect(
        master.dataBytes / (static_cast<std::uint64_t>(channels) * sizeof(float)) == expectedFrames,
        "master frame count mismatch");

    // CodecDelay describes leading codec priming. The skipped samples are
    // removed from the beginning; terminal decoded signal must be retained.
    constexpr std::size_t comparisonFrames = 65536;
    const std::uint64_t comparisonStart = (std::min)(
        expectedFrames / 3,
        expectedFrames - comparisonFrames - static_cast<std::uint64_t>(expectedSkip) - 1);
    const auto masterWindow = readFloatFrames(
        masterPath, master.dataOffset, comparisonStart, comparisonFrames, channels);
    const auto alignedWindow = readFloatFrames(
        outputPcm, 0, comparisonStart, comparisonFrames, channels);
    const auto shiftedWindow = readFloatFrames(
        outputPcm, 0, comparisonStart + expectedSkip, comparisonFrames, channels);
    const double alignedCorrelation = normalizedCorrelation(masterWindow, alignedWindow);
    const double shiftedCorrelation = normalizedCorrelation(masterWindow, shiftedWindow);
    ok &= expect(alignedCorrelation > 0.90, "decoded PCM is not aligned to the master");
    ok &= expect(
        alignedCorrelation > shiftedCorrelation + 0.10,
        "leading priming still provides the stronger alignment");

    constexpr std::size_t tailFrames = 1024;
    const auto masterTail = readFloatFrames(
        masterPath, master.dataOffset, expectedFrames - tailFrames, tailFrames, channels);
    const auto outputTail = readFloatFrames(
        outputPcm, 0, expectedFrames - tailFrames, tailFrames, channels);
    const double tailCorrelation = normalizedCorrelation(masterTail, outputTail);
    const double tailRms = rms(outputTail);
    ok &= expect(tailCorrelation > 0.90, "terminal decoded signal no longer matches the master tail");
    ok &= expect(tailRms > 1e-4, "terminal useful signal was replaced by silence");

    std::cout << "matroskaCodecDelayFrames=" << codecDelayFrames
              << " skipSamplesStart=" << evidence.gapless.skipSamplesStart
              << " physicalFrames=" << evidence.packetTiming.aacFrameCountCandidateFrames
              << " rawProbeFrames=" << expectedRawProbeFrames
              << " initialPresentationFrames=" << (probeFrames ? *probeFrames : 0)
              << " firstImportProgressEstimateFrames=" << progressCapture.firstEstimatedTotalFrames
              << " decodedPresentationFrames=" << expectedFrames
              << " presentationDurationSec=" << presentationDuration
              << " alignedCorrelation=" << alignedCorrelation
              << " shiftedCorrelation=" << shiftedCorrelation
              << " tailCorrelation=" << tailCorrelation
              << " tailRms=" << tailRms << "\n";
    if (ok) {
        std::cout << "[AVEMEDIABRIDGE_LOADING_PRESENTATION_EVIDENCE_DIAG]"
                  << " packetScanReachedEof=yes"
                  << " packetFrameCountExact=yes"
                  << " packetFrameCount=" << evidence.packetTiming.codecFrameCountFrames
                  << " initialSkipAuthoritative=yes"
                  << " initialSkipSamples=" << exactEvidence.initialSkipFrames
                  << " terminalDiscardKnown=yes"
                  << " terminalDiscardSamples=" << exactEvidence.terminalDiscardFrames
                  << " candidatePresentationFrames=" << exactBudget.presentationFrames
                  << " oldBudgetAccepted=no"
                  << " oldBudgetRejectReason=stream_duration_not_sample_exact"
                  << " oldInitialPresentationExtent=" << expectedRawProbeFrames
                  << " firstLoadingPresentationFrames=" << (probeFrames ? *probeFrames : 0)
                  << " firstImportProgressEstimateFrames=" << progressCapture.firstEstimatedTotalFrames
                  << " readyAuthoritativeFrames=" << expectedFrames << "\n";
        std::cout << "AVEMEDIABRIDGE_MATROSKA_LOADING_READY_PRESENTATION_PARITY_OK\n";
        std::cout << "AVEMEDIABRIDGE_MATROSKA_CODEC_DELAY_SKIP_OK\n";
        return 0;
    }
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        std::cout << "AVEMEDIABRIDGE_MATROSKA_CODEC_DELAY_SKIP_SKIPPED: "
                     "media, master, expected frames, expected skip, and raw probe frames are required\n";
        return 77;
    }

    const std::filesystem::path mediaPath(argv[1]);
    const std::filesystem::path masterPath(argv[2]);
    if (!std::filesystem::is_regular_file(mediaPath) ||
        !std::filesystem::is_regular_file(masterPath)) {
        std::cout << "AVEMEDIABRIDGE_MATROSKA_CODEC_DELAY_SKIP_SKIPPED: external media unavailable\n";
        return 77;
    }

    try {
        return run(
            mediaPath,
            masterPath,
            std::stoull(argv[3]),
            std::stoll(argv[4]),
            std::stoull(argv[5]));
    } catch (const std::exception& error) {
        std::cerr << "matroskaCodecDelaySkipTest: exception=\"" << error.what() << "\"\n";
        return 1;
    }
}
