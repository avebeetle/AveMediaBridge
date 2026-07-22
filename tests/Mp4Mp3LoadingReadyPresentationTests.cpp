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

class ScopedTempDirectory {
public:
    ScopedTempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            (L"avemediabridge_mp4_mp3_presentation_" +
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

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "mp4Mp3LoadingReadyPresentationTest: " << message << '\n';
    }
    return condition;
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to read JSON artifact");
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::optional<double> jsonNumber(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    const std::size_t keyAt = json.find(token);
    const std::size_t colon = keyAt == std::string::npos
        ? std::string::npos
        : json.find(':', keyAt + token.size());
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    try {
        return std::stod(json.substr(colon + 1));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> jsonString(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    const std::size_t keyAt = json.find(token);
    const std::size_t colon = keyAt == std::string::npos
        ? std::string::npos
        : json.find(':', keyAt + token.size());
    const std::size_t quote = colon == std::string::npos
        ? std::string::npos
        : json.find('"', colon + 1);
    const std::size_t end = quote == std::string::npos
        ? std::string::npos
        : json.find('"', quote + 1);
    return end == std::string::npos
        ? std::nullopt
        : std::optional<std::string>{json.substr(quote + 1, end - quote - 1)};
}

std::optional<bool> jsonBool(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    std::size_t cursor = json.find(token);
    cursor = cursor == std::string::npos
        ? cursor
        : json.find(':', cursor + token.size());
    cursor = cursor == std::string::npos
        ? cursor
        : json.find_first_not_of(" \t\r\n", cursor + 1);
    if (cursor != std::string::npos && json.compare(cursor, 4, "true") == 0) {
        return true;
    }
    if (cursor != std::string::npos && json.compare(cursor, 5, "false") == 0) {
        return false;
    }
    return std::nullopt;
}

std::vector<float> readTailFrames(
    const std::filesystem::path& path,
    std::uint64_t totalFrames,
    std::size_t frameCount,
    int channels) {
    std::ifstream input(path, std::ios::binary);
    if (!input || totalFrames < frameCount || channels <= 0) {
        throw std::runtime_error("invalid PCM tail request");
    }
    const std::uint64_t firstSample =
        (totalFrames - frameCount) * static_cast<std::uint64_t>(channels);
    input.seekg(static_cast<std::streamoff>(firstSample * sizeof(float)));
    std::vector<float> samples(frameCount * static_cast<std::size_t>(channels));
    input.read(
        reinterpret_cast<char*>(samples.data()),
        static_cast<std::streamsize>(samples.size() * sizeof(float)));
    if (input.gcount() != static_cast<std::streamsize>(samples.size() * sizeof(float))) {
        throw std::runtime_error("PCM tail is incomplete");
    }
    return samples;
}

struct ProgressCapture {
    std::uint64_t firstEstimatedFrames = 0;
};

void AVEMEDIABRIDGE_CALL captureProgress(
    const AveMediaBridgeImportProgress* progress,
    void* userData) {
    if (!progress || !userData) {
        return;
    }
    auto& capture = *static_cast<ProgressCapture*>(userData);
    if (capture.firstEstimatedFrames == 0 && progress->estimatedTotalFrames > 0) {
        capture.firstEstimatedFrames = progress->estimatedTotalFrames;
    }
}

int run(const std::filesystem::path& mediaPath) {
    constexpr std::int64_t expectedPhysicalFrames = 105841152;
    constexpr std::int64_t expectedInitialSkip = 1105;
    constexpr std::int64_t expectedTerminalDiscard = 47;
    constexpr std::int64_t expectedPresentationFrames = 105840000;

    AVFormatContext* rawFormat = nullptr;
    const std::string mediaUtf8 = mediaPath.u8string();
    int result = avformat_open_input(&rawFormat, mediaUtf8.c_str(), nullptr, nullptr);
    if (result < 0) {
        throw std::runtime_error("failed to open MP4/MP3 fixture");
    }
    Ffmpeg::UniqueAVFormatContext format(rawFormat);
    if (avformat_find_stream_info(format.get(), nullptr) < 0) {
        throw std::runtime_error("failed to inspect MP4/MP3 fixture");
    }
    const int audioStreamIndex = av_find_best_stream(
        format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioStreamIndex < 0) {
        throw std::runtime_error("MP4/MP3 fixture has no selected audio stream");
    }
    const AVCodecParameters* codec = format->streams[audioStreamIndex]->codecpar;
    const int sampleRate = codec->sample_rate;
    const int channels = codec->ch_layout.nb_channels;
    const AVCodecID codecId = codec->codec_id;
    const int preliminaryInitialPadding = codec->initial_padding;
    const int preliminaryTrailingPadding = codec->trailing_padding;
    format.reset();

    constexpr Probe::PacketScanOptions scanOptions{
        4LL * 1024LL * 1024LL,
        3LL * AV_TIME_BASE
    };
    const Probe::AudioPresentationEvidenceScan scan =
        Probe::scanAudioPresentationEvidence(
            mediaUtf8, audioStreamIndex, sampleRate, codecId, scanOptions);
    const Probe::ExactPacketPresentationEvidence exactEvidence =
        Probe::makeExactPacketPresentationEvidence(scan);
    const Probe::ExactPacketPresentationBudget exactBudget =
        Probe::resolveExactPacketPresentationBudget(exactEvidence);
    const std::int64_t legacyCandidate =
        scan.packetTiming.mp3FrameCountCandidateFrames - scan.gapless.skipSamplesStart;

    bool ok = true;
    ok &= expect(codecId == AV_CODEC_ID_MP3, "selected codec is not MP3");
    ok &= expect(sampleRate == 44100 && channels == 2, "fixture audio shape changed");
    ok &= expect(preliminaryInitialPadding == 0, "pre-scan initial padding changed");
    ok &= expect(preliminaryTrailingPadding == 0, "pre-scan trailing padding changed");
    ok &= expect(scan.packetTiming.reachedEof, "packet accumulator did not reach EOF");
    ok &= expect(scan.gapless.reachedEof, "gapless accumulator did not reach EOF");
    ok &= expect(scan.packetTiming.warning.empty(), "packet accumulator warning");
    ok &= expect(scan.gapless.warning.empty(), "gapless accumulator warning");
    ok &= expect(
        scan.packetTiming.codecFrameCountFrames == expectedPhysicalFrames,
        "physical MP3 frame count changed");
    ok &= expect(
        exactEvidence.initialSkipFrames == expectedInitialSkip,
        "authoritative initial skip changed");
    ok &= expect(
        exactEvidence.terminalDiscardFrames == expectedTerminalDiscard,
        "authoritative terminal discard changed");
    ok &= expect(exactBudget.accepted, "exact packet presentation was rejected");
    ok &= expect(
        exactBudget.presentationFrames == expectedPresentationFrames,
        "exact packet presentation count changed");
    ok &= expect(legacyCandidate == 105840047, "legacy candidate changed");

    ScopedTempDirectory temp;
    const std::filesystem::path mediaDir = temp.path() / L"Media";
    std::filesystem::create_directories(mediaDir);
    const std::filesystem::path probePath = mediaDir / L"probe.json";
    result = AveMediaBridge_ProbeToJson(mediaPath.c_str(), probePath.c_str());
    ok &= expect(result == 0, "AveMediaBridge probe failed");
    const std::string probeJson = result == 0 ? readText(probePath) : std::string{};
    const auto probeFrames = jsonNumber(probeJson, "decodedSampleFrames");
    const auto probeKind = jsonString(probeJson, "decodedSampleFramesKind");
    const auto probeSource = jsonString(probeJson, "decodedSampleFramesSource");
    const auto probeSkipStart = jsonNumber(probeJson, "skipSamplesStart");
    const auto probeSkipEnd = jsonNumber(probeJson, "skipSamplesEnd");
    const auto sampleTableStatus = jsonString(probeJson, "mp4Mp3SampleTableStatus");
    const auto genericScanEntered = jsonBool(probeJson, "mp4Mp3SampleTableGenericScanEntered");
    const auto genericScanSkipped = jsonBool(probeJson, "mp4Mp3SampleTableGenericScanSkipped");
    ok &= expect(
        probeFrames && *probeFrames == expectedPresentationFrames,
        "initial Loading extent is not exact");
    ok &= expect(probeKind && *probeKind == "exact", "probe authority kind is not exact");
    ok &= expect(
        probeSource && *probeSource == "mp4_mp3_sample_edit_table_presentation",
        "sample/edit-table authority did not take precedence");
    ok &= expect(probeSkipStart && *probeSkipStart == expectedInitialSkip, "probe start skip changed");
    ok &= expect(probeSkipEnd && *probeSkipEnd == expectedTerminalDiscard, "probe tail discard changed");
    ok &= expect(sampleTableStatus && *sampleTableStatus == "exact", "sample-table proof is not exact");
    ok &= expect(genericScanEntered && !*genericScanEntered, "generic MP4/MP3 scan still entered");
    ok &= expect(genericScanSkipped && *genericScanSkipped, "generic MP4/MP3 scan was not skipped");

    ProgressCapture progress;
    AveMediaBridgeImportOptions options{};
    options.structSize = sizeof(options);
    options.inputPath = mediaPath.c_str();
    options.sessionMediaDir = mediaDir.c_str();
    options.onProgress = captureProgress;
    options.userData = &progress;
    result = AveMediaBridge_ImportAudioToSessionEx(&options);
    ok &= expect(result == 0, "AveMediaBridge import failed");
    if (!ok) {
        return 1;
    }

    const std::string audioInfo = readText(mediaDir / L"audio_info.json");
    const std::string metadata = readText(mediaDir / L"metadata.json");
    const auto readyFrames = jsonNumber(audioInfo, "frames");
    const auto reopenFrames = jsonNumber(readText(mediaDir / L"audio_info.json"), "frames");
    const std::filesystem::path pcmPath = mediaDir / L"original_f32.bin";
    const auto budgetSource = jsonString(metadata, "source");
    const auto packetScanExecuted = jsonBool(metadata, "packetScanExecuted");
    const auto handoffAccepted = jsonString(metadata, "mp4Mp3SampleTableHandoffStatus");
    const auto authorityReused = jsonBool(metadata, "mp4Mp3SampleTableAuthorityReused");
    ok &= expect(
        readyFrames && *readyFrames == expectedPresentationFrames,
        "Ready frame authority changed");
    ok &= expect(
        reopenFrames && *reopenFrames == expectedPresentationFrames,
        "reopen frame authority changed");
    ok &= expect(
        std::filesystem::file_size(pcmPath) ==
            static_cast<std::uint64_t>(expectedPresentationFrames) * channels * sizeof(float),
        "final PCM byte count changed");
    ok &= expect(
        budgetSource && *budgetSource == "mp4_mp3_sample_edit_table_presentation",
        "streaming presentation budget did not use sample-table authority");
    ok &= expect(packetScanExecuted && !*packetScanExecuted,
                 "import repeated the generic packet scan");
    ok &= expect(handoffAccepted && *handoffAccepted == "accepted",
                 "probe-to-import handoff was not accepted");
    ok &= expect(authorityReused && *authorityReused,
                 "import did not reuse the exact probe authority");

    const std::vector<float> lastValidFrames = readTailFrames(
        pcmPath, expectedPresentationFrames, expectedTerminalDiscard, channels);
    const bool lastValidSignalPresent = std::any_of(
        lastValidFrames.begin(), lastValidFrames.end(), [](float sample) {
            return std::isfinite(sample) && std::fabs(sample) > 1e-5F;
        });
    ok &= expect(lastValidSignalPresent, "last valid 47-frame signal was not retained");

    std::cout << "preliminaryInitialPadding=" << preliminaryInitialPadding
              << " preliminaryTrailingPadding=" << preliminaryTrailingPadding
              << " oracleCombinedScanExecuted=yes"
              << " productCombinedScanExecuted=no"
              << " physicalFrames=" << scan.packetTiming.codecFrameCountFrames
              << " initialSkip=" << exactEvidence.initialSkipFrames
              << " terminalDiscard=" << exactEvidence.terminalDiscardFrames
              << " legacyCandidate=" << legacyCandidate
              << " exactCandidate=" << exactBudget.presentationFrames
              << " firstLoadingFrames=" << (probeFrames ? *probeFrames : 0)
              << " firstImportProgressEstimate=" << progress.firstEstimatedFrames
              << " readyFrames=" << (readyFrames ? *readyFrames : 0)
              << " reopenFrames=" << (reopenFrames ? *reopenFrames : 0) << '\n';
    if (ok) {
        std::cout << "AVEMEDIABRIDGE_MP4_MP3_LOADING_READY_PRESENTATION_PARITY_OK\n";
        return 0;
    }
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cout << "AVEMEDIABRIDGE_MP4_MP3_LOADING_READY_PRESENTATION_PARITY_SKIPPED: "
                     "external media path is required\n";
        return 77;
    }
    const std::filesystem::path mediaPath(argv[1]);
    if (!std::filesystem::is_regular_file(mediaPath)) {
        std::cout << "AVEMEDIABRIDGE_MP4_MP3_LOADING_READY_PRESENTATION_PARITY_SKIPPED: "
                     "external media unavailable\n";
        return 77;
    }
    try {
        return run(mediaPath);
    } catch (const std::exception& error) {
        std::cerr << "mp4Mp3LoadingReadyPresentationTest: exception=\""
                  << error.what() << "\"\n";
        return 1;
    }
}
