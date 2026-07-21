#include "include/AveMediaBridge/AveMediaBridgeApi.hpp"
#include "src/Probe/FlacStreamInfo.hpp"
#include "src/Probe/PresentationBudgetPolicy.hpp"

#include <array>
#include <chrono>
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
#include <bcrypt.h>

namespace {

namespace Probe = AveMediaBridge::Probe;

class ScopedTempDirectory {
public:
    explicit ScopedTempDirectory(const wchar_t* label) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            (std::wstring(label) + L"_" + std::to_wstring(GetCurrentProcessId()) +
             L"_" + std::to_wstring(stamp));
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

bool expect(bool condition, const std::string& name, const std::string& detail = {}) {
    if (condition) {
        return true;
    }
    std::cerr << "flacStreamInfoAuthorityTest: case=\"" << name << "\"";
    if (!detail.empty()) {
        std::cerr << " detail=\"" << detail << "\"";
    }
    std::cerr << '\n';
    return false;
}

std::array<std::uint8_t, 34> makeStreamInfo(
    int sampleRate,
    int channels,
    int bitsPerSample,
    std::uint64_t totalSamples) {
    std::array<std::uint8_t, 34> bytes{};
    bytes[0] = 0x10;
    bytes[2] = 0x10;
    const std::uint64_t packed =
        (static_cast<std::uint64_t>(sampleRate) << 44U) |
        (static_cast<std::uint64_t>(channels - 1) << 41U) |
        (static_cast<std::uint64_t>(bitsPerSample - 1) << 36U) |
        (totalSamples & ((std::uint64_t{1} << 36U) - 1U));
    for (int index = 0; index < 8; ++index) {
        bytes[10 + index] = static_cast<std::uint8_t>(packed >> (56U - index * 8U));
    }
    return bytes;
}

AVCodecParameters makeCodecpar(
    AVCodecID codecId,
    std::uint8_t* extradata,
    int extradataSize,
    int sampleRate,
    int channels) {
    AVCodecParameters codecpar{};
    codecpar.codec_id = codecId;
    codecpar.codec_type = AVMEDIA_TYPE_AUDIO;
    codecpar.extradata = extradata;
    codecpar.extradata_size = extradataSize;
    codecpar.sample_rate = sampleRate;
    codecpar.ch_layout.nb_channels = channels;
    return codecpar;
}

Probe::TotalPresentationEvidence makeExactEvidence(
    std::uint64_t frames,
    Probe::PresentationTotalSource source,
    Probe::PresentationTotalValidation validation) {
    Probe::TotalPresentationEvidence evidence;
    evidence.frames = frames;
    evidence.trust = Probe::PresentationTotalTrust::SampleExact;
    evidence.source = source;
    evidence.domain = Probe::PresentationSampleDomain::NativeStreamSamples;
    evidence.sampleRate = 48000;
    evidence.exactRescale = true;
    evidence.validation = validation;
    return evidence;
}

Probe::TotalPresentationEvidence evidenceFor(
    AVCodecParameters& codecpar,
    std::int64_t duration,
    AVRational timeBase) {
    AVStream stream{};
    stream.codecpar = &codecpar;
    stream.duration = duration;
    stream.time_base = timeBase;
    return Probe::makeStreamTotalPresentationEvidence(nullptr, &stream);
}

bool runTruthTable() {
    std::size_t passed = 0;
    auto check = [&](bool condition, const char* name) {
        const bool ok = expect(condition, name);
        passed += ok ? 1U : 0U;
        return ok;
    };

    bool ok = true;
    auto positive = makeStreamInfo(48000, 2, 24, 102400);
    auto codecpar = makeCodecpar(
        AV_CODEC_ID_FLAC, positive.data(), static_cast<int>(positive.size()), 48000, 2);
    const Probe::TotalPresentationEvidence positiveEvidence = evidenceFor(
        codecpar, AV_NOPTS_VALUE, AVRational{1, 1000});
    Probe::StreamingPresentationBudgetInput noScanBudget;
    noScanBudget.streamTotal = positiveEvidence;
    const Probe::StreamingPresentationBudgetDecision noScanDecision =
        Probe::resolveStreamingPresentationBudget(noScanBudget);
    ok &= check(
        positiveEvidence.trust == Probe::PresentationTotalTrust::SampleExact &&
            positiveEvidence.frames == 102400 && noScanDecision.accepted &&
            noScanDecision.frames == 102400,
        "valid_positive_without_stream_duration");

    auto zero = makeStreamInfo(48000, 2, 24, 0);
    codecpar = makeCodecpar(AV_CODEC_ID_FLAC, zero.data(), 34, 48000, 2);
    ok &= check(
        Probe::parseFlacStreamInfo(&codecpar).status ==
            Probe::FlacStreamInfoStatus::TotalSamplesUnknown &&
            evidenceFor(codecpar, AV_NOPTS_VALUE, AVRational{1, 1000}).trust ==
                Probe::PresentationTotalTrust::Unknown,
        "zero_total_unknown");

    codecpar = makeCodecpar(AV_CODEC_ID_FLAC, positive.data(), 20, 48000, 2);
    ok &= check(
        Probe::parseFlacStreamInfo(&codecpar).status ==
            Probe::FlacStreamInfoStatus::TruncatedExtradata,
        "truncated_extradata");

    std::array<std::uint8_t, 42> unsupported{};
    codecpar = makeCodecpar(AV_CODEC_ID_FLAC, unsupported.data(), 42, 48000, 2);
    ok &= check(
        Probe::parseFlacStreamInfo(&codecpar).status ==
            Probe::FlacStreamInfoStatus::UnsupportedLayout,
        "unsupported_layout");

    codecpar = makeCodecpar(AV_CODEC_ID_FLAC, positive.data(), 34, 44100, 2);
    ok &= check(
        Probe::parseFlacStreamInfo(&codecpar).status ==
            Probe::FlacStreamInfoStatus::SampleRateMismatch,
        "sample_rate_conflict");

    codecpar = makeCodecpar(AV_CODEC_ID_FLAC, positive.data(), 34, 48000, 1);
    ok &= check(
        Probe::parseFlacStreamInfo(&codecpar).status ==
            Probe::FlacStreamInfoStatus::ChannelCountMismatch,
        "channel_count_conflict");

    codecpar = makeCodecpar(AV_CODEC_ID_FLAC, positive.data(), 34, 48000, 2);
    const auto roundedMismatch = evidenceFor(codecpar, 2133, AVRational{1, 1000});
    ok &= check(
        roundedMismatch.frames == 102400 && !roundedMismatch.conflict &&
            roundedMismatch.source == Probe::PresentationTotalSource::FlacStreamInfoTotalSamples,
        "rounded_container_duration_is_diagnostic");

    const auto sourceExact = makeExactEvidence(
        102400,
        Probe::PresentationTotalSource::FlacStreamInfoTotalSamples,
        Probe::PresentationTotalValidation::SelfContainedMetadata);
    const auto equalDuration = makeExactEvidence(
        102400,
        Probe::PresentationTotalSource::ExactPcmStreamDuration,
        Probe::PresentationTotalValidation::PacketCrossCheckRequired);
    ok &= check(
        !Probe::reconcileTotalPresentationEvidence(sourceExact, equalDuration).conflict,
        "equal_independent_exact_total");

    const auto conflictingDuration = makeExactEvidence(
        102384,
        Probe::PresentationTotalSource::ExactPcmStreamDuration,
        Probe::PresentationTotalValidation::PacketCrossCheckRequired);
    const auto conflict = Probe::reconcileTotalPresentationEvidence(
        sourceExact, conflictingDuration);
    Probe::StreamingPresentationBudgetInput conflictBudget;
    conflictBudget.streamTotal = conflict;
    ok &= check(
        conflict.conflict &&
            !Probe::resolveStreamingPresentationBudget(conflictBudget).accepted,
        "conflicting_independent_exact_total");

    bool targetPatterns = true;
    for (const auto& pattern : std::array<std::pair<std::uint64_t, std::int64_t>, 3>{
             std::pair<std::uint64_t, std::int64_t>{102400, 2133},
             {103423, 2155},
             {28801023, 600021}}) {
        auto bytes = makeStreamInfo(48000, 2, 24, pattern.first);
        auto targetCodecpar = makeCodecpar(AV_CODEC_ID_FLAC, bytes.data(), 34, 48000, 2);
        const auto targetEvidence = evidenceFor(
            targetCodecpar, pattern.second, AVRational{1, 1000});
        targetPatterns &= targetEvidence.frames == pattern.first &&
            targetEvidence.trust == Probe::PresentationTotalTrust::SampleExact;
    }
    ok &= check(targetPatterns, "EA069_EA085_EA094_patterns");

    auto ea020 = makeStreamInfo(16000, 2, 24, 257);
    codecpar = makeCodecpar(AV_CODEC_ID_FLAC, ea020.data(), 34, 16000, 2);
    ok &= check(
        evidenceFor(codecpar, 16, AVRational{1, 1000}).frames == 257,
        "EA020_pattern");

    codecpar = makeCodecpar(AV_CODEC_ID_AAC, nullptr, 0, 48000, 2);
    const auto nonFlac = evidenceFor(codecpar, 1024, AVRational{1, 48000});
    ok &= check(
        nonFlac.trust == Probe::PresentationTotalTrust::Estimated &&
            nonFlac.source == Probe::PresentationTotalSource::StreamDurationEstimate,
        "non_flac_unchanged");

    return ok && expect(passed == 12, "truth_table_count");
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to read artifact");
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::optional<std::uint64_t> jsonUnsigned(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    const std::size_t keyAt = json.find(token);
    const std::size_t colon = keyAt == std::string::npos
        ? std::string::npos
        : json.find(':', keyAt + token.size());
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    try {
        return static_cast<std::uint64_t>(std::stoull(json.substr(colon + 1)));
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
    const std::size_t keyAt = json.find(token);
    const std::size_t colon = keyAt == std::string::npos
        ? std::string::npos
        : json.find(':', keyAt + token.size());
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t value = json.find_first_not_of(" \t\r\n", colon + 1);
    if (value != std::string::npos && json.compare(value, 4, "true") == 0) {
        return true;
    }
    if (value != std::string::npos && json.compare(value, 5, "false") == 0) {
        return false;
    }
    return std::nullopt;
}

std::string sha256File(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0;
    DWORD digestBytes = 0;
    DWORD copied = 0;
    std::vector<UCHAR> object;
    std::vector<UCHAR> digest;
    auto close = [&]() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    };
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &copied, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&digestBytes), sizeof(digestBytes), &copied, 0) < 0) {
        close();
        throw std::runtime_error("failed to initialize SHA-256");
    }
    object.resize(objectBytes);
    digest.resize(digestBytes);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) < 0) {
        close();
        throw std::runtime_error("failed to create SHA-256");
    }
    std::ifstream input(path, std::ios::binary);
    std::vector<char> buffer(1024 * 1024);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0 && BCryptHashData(hash,
                reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) < 0) {
            close();
            throw std::runtime_error("failed to update SHA-256");
        }
    }
    if (BCryptFinishHash(hash, digest.data(), digestBytes, 0) < 0) {
        close();
        throw std::runtime_error("failed to finish SHA-256");
    }
    close();
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (UCHAR byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

struct ProgressCapture {
    std::uint64_t firstEstimatedFrames = 0;
    std::uint64_t lastEstimatedFrames = 0;
};

void AVEMEDIABRIDGE_CALL captureProgress(
    const AveMediaBridgeImportProgress* progress,
    void* userData) {
    if (!progress || !userData || progress->estimatedTotalFrames == 0) {
        return;
    }
    auto& capture = *static_cast<ProgressCapture*>(userData);
    if (capture.firstEstimatedFrames == 0) {
        capture.firstEstimatedFrames = progress->estimatedTotalFrames;
    }
    capture.lastEstimatedFrames = progress->estimatedTotalFrames;
}

struct RealCase {
    const char* id;
    std::uint64_t frames;
    int sampleRate;
    int channels;
    const char* pcmSha256;
};

bool runRealCase(const std::filesystem::path& media, const RealCase& expected) {
    ScopedTempDirectory temp(L"avemediabridge_flac_streaminfo");
    const std::filesystem::path probePath = temp.path() / L"probe.json";
    const std::filesystem::path mediaDir = temp.path() / L"Media";
    bool ok = true;

    int result = AveMediaBridge_ProbeToJson(media.c_str(), probePath.c_str());
    ok &= expect(result == 0, expected.id, "probe failed");
    const std::string probe = result == 0 ? readText(probePath) : std::string{};
    const auto probeFrames = jsonUnsigned(probe, "decodedSampleFrames");
    const auto probeTrust = jsonString(probe, "decodedSampleFramesTrust");
    const auto probeSource = jsonString(probe, "decodedSampleFramesSource");
    ok &= expect(probeFrames && *probeFrames == expected.frames, expected.id, "probe frames");
    ok &= expect(probeTrust && *probeTrust == "authoritative", expected.id, "probe trust");
    ok &= expect(
        probeSource && *probeSource == "flac_streaminfo_total_samples",
        expected.id,
        "probe source");

    ProgressCapture progress;
    AveMediaBridgeImportOptions options{};
    options.structSize = sizeof(options);
    options.inputPath = media.c_str();
    options.sessionMediaDir = mediaDir.c_str();
    options.onProgress = captureProgress;
    options.userData = &progress;
    result = AveMediaBridge_ImportAudioToSessionEx(&options);
    ok &= expect(result == 0, expected.id, "import failed");
    if (result != 0) {
        return false;
    }

    const std::string audioInfo = readText(mediaDir / L"audio_info.json");
    const std::string metadata = readText(mediaDir / L"metadata.json");
    const auto readyFrames = jsonUnsigned(audioInfo, "frames");
    const auto budgetFrames = jsonUnsigned(metadata, "frames");
    const auto physicalFrames = jsonUnsigned(metadata, "physicalInputFrames");
    const auto writtenFrames = jsonUnsigned(metadata, "writtenOutputFrames");
    const auto budgetSource = jsonString(metadata, "source");
    const auto validation = jsonString(metadata, "validation");
    const auto packetScanExecuted = jsonBool(metadata, "packetScanExecuted");
    const std::filesystem::path pcm = mediaDir / L"original_f32.bin";
    const std::string pcmHash = sha256File(pcm);

    ok &= expect(
        progress.firstEstimatedFrames == expected.frames &&
            progress.lastEstimatedFrames == expected.frames,
        expected.id,
        "progress extent");
    ok &= expect(readyFrames && *readyFrames == expected.frames, expected.id, "Ready frames");
    ok &= expect(budgetFrames && *budgetFrames == expected.frames, expected.id, "budget frames");
    ok &= expect(physicalFrames && *physicalFrames == expected.frames, expected.id, "physical frames");
    ok &= expect(writtenFrames && *writtenFrames == expected.frames, expected.id, "written frames");
    ok &= expect(
        budgetSource && *budgetSource == "flac_streaminfo_total_samples",
        expected.id,
        "budget source");
    ok &= expect(
        validation && *validation == "self_contained_metadata",
        expected.id,
        "validation mode");
    ok &= expect(
        packetScanExecuted && !*packetScanExecuted,
        expected.id,
        "packet scan added");
    ok &= expect(
        std::filesystem::file_size(pcm) ==
            expected.frames * static_cast<std::uint64_t>(expected.channels) * sizeof(float),
        expected.id,
        "PCM byte count");
    ok &= expect(pcmHash == expected.pcmSha256, expected.id, "PCM hash");

    std::cout << "case=" << expected.id
              << " streamInfoTotal=" << expected.frames
              << " probeFrames=" << (probeFrames ? *probeFrames : 0)
              << " firstLoadingFrames=" << progress.firstEstimatedFrames
              << " readyFrames=" << (readyFrames ? *readyFrames : 0)
              << " reopenFrames=" << (readyFrames ? *readyFrames : 0)
              << " physicalFrames=" << (physicalFrames ? *physicalFrames : 0)
              << " writtenFrames=" << (writtenFrames ? *writtenFrames : 0)
              << " packetScanExecuted="
              << (packetScanExecuted && *packetScanExecuted ? "yes" : "no")
              << " pcmSha256=" << pcmHash << '\n';
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cout << "AVEMEDIABRIDGE_FLAC_STREAMINFO_PRESENTATION_AUTHORITY_SKIPPED: "
                     "EA020, EA069, EA085, and EA094 paths are required\n";
        return 77;
    }
    for (int index = 1; index < argc; ++index) {
        if (!std::filesystem::is_regular_file(std::filesystem::path(argv[index]))) {
            std::cout << "AVEMEDIABRIDGE_FLAC_STREAMINFO_PRESENTATION_AUTHORITY_SKIPPED: "
                         "external media unavailable\n";
            return 77;
        }
    }

    try {
        bool ok = runTruthTable();
        const std::array<RealCase, 4> cases{
            RealCase{"EA020", 257, 16000, 2,
                "b2b3996848dae9aa0b349ef477bda0e2de51a95cab4f80073d93282c48121dff"},
            RealCase{"EA069", 102400, 48000, 2,
                "32d55d3cedb21cd93281bc7c742260cd495530bcde6fec9deaef5bca8ff4d520"},
            RealCase{"EA085", 103423, 48000, 2,
                "7fe5db1bf9132cbf4099cc54293d40e18b2751cb698c83e4f81e2b59d6b20c63"},
            RealCase{"EA094", 28801023, 48000, 2,
                "a814e13c8aa548ee147efb52707511c42c3b46e2758515c6c2563c7ad8382d5a"},
        };
        for (int index = 0; index < 4; ++index) {
            ok &= runRealCase(std::filesystem::path(argv[index + 1]), cases[index]);
        }
        if (!ok) {
            return 1;
        }
        std::cout << "truthTableCases=12 truthTablePasses=12\n";
        std::cout << "AVEMEDIABRIDGE_FLAC_STREAMINFO_PRESENTATION_AUTHORITY_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "flacStreamInfoAuthorityTest: exception=\""
                  << error.what() << "\"\n";
        return 1;
    }
}
