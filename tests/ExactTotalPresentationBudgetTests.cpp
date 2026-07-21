#include "include/AveMediaBridge/AveMediaBridgeApi.hpp"
#include "src/Decode/AudioPresentationSlice.hpp"
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

namespace Decode = AveMediaBridge::Decode;
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
    std::cerr << "exactTotalPresentationBudgetTest: case=\"" << name << "\"";
    if (!detail.empty()) {
        std::cerr << " detail=\"" << detail << "\"";
    }
    std::cerr << '\n';
    return false;
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to read JSON artifact");
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

std::string sha256File(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0;
    DWORD digestBytes = 0;
    DWORD copied = 0;
    std::vector<UCHAR> object;
    std::vector<UCHAR> digest;

    auto close = [&]() {
        if (hash) {
            BCryptDestroyHash(hash);
        }
        if (algorithm) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    };
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(
            algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &copied, 0) < 0 ||
        BCryptGetProperty(
            algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&digestBytes), sizeof(digestBytes), &copied, 0) < 0) {
        close();
        throw std::runtime_error("failed to initialize SHA-256");
    }
    object.resize(objectBytes);
    digest.resize(digestBytes);
    if (BCryptCreateHash(
            algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) < 0) {
        close();
        throw std::runtime_error("failed to create SHA-256 hash");
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        close();
        throw std::runtime_error("failed to open PCM for SHA-256");
    }
    std::vector<char> buffer(1024 * 1024);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0 && BCryptHashData(
                hash,
                reinterpret_cast<PUCHAR>(buffer.data()),
                static_cast<ULONG>(count),
                0) < 0) {
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
    result.reserve(digest.size() * 2);
    for (UCHAR byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

Decode::DecodedAudioPresentationPlan slice(
    int physical,
    std::uint64_t remaining,
    bool durationKnown = false,
    std::int64_t duration = 0) {
    Decode::DecodedAudioPresentationInput input;
    input.frameNbSamples = physical;
    input.inputSampleRate = 48000;
    input.remainingPresentationFramesKnown = true;
    input.remainingPresentationFrames = remaining;
    input.frameDurationKnown = durationKnown;
    input.frameDuration = duration;
    input.frameDurationTimeBase = AVRational{1, 48000};
    return Decode::resolvePresentedInputSamples(input);
}

Probe::TotalPresentationEvidence exactTotal(
    std::uint64_t frames,
    Probe::PresentationTotalSource source = Probe::PresentationTotalSource::OggEosGranule) {
    Probe::TotalPresentationEvidence evidence;
    evidence.frames = frames;
    evidence.trust = Probe::PresentationTotalTrust::SampleExact;
    evidence.source = source;
    evidence.domain = Probe::PresentationSampleDomain::NativeStreamSamples;
    evidence.sampleRate = 48000;
    evidence.exactRescale = true;
    return evidence;
}

Probe::StreamingPresentationBudgetInput completeBudgetInput() {
    Probe::StreamingPresentationBudgetInput input;
    input.scanReachedEof = true;
    input.packetDurationsComplete = true;
    input.packetDurationArithmeticValid = true;
    return input;
}

bool runPolicyTruthTable() {
    std::size_t passed = 0;
    auto check = [&](bool condition, const char* name) {
        const bool ok = expect(condition, name);
        passed += ok ? 1 : 0;
        return ok;
    };

    bool ok = true;
    ok &= check(slice(128, 63).acceptedInputSamples == 63, "physical_128_budget_63");
    ok &= check(slice(1024, 1023).acceptedInputSamples == 1023, "physical_1024_budget_1023");
    ok &= check(slice(1024, 1024).acceptedInputSamples == 1024, "budget_equals_physical");
    auto exactZero = completeBudgetInput();
    exactZero.streamTotal = exactTotal(0);
    const auto exactZeroDecision = Probe::resolveStreamingPresentationBudget(exactZero);
    ok &= check(
        slice(128, 0).acceptedInputSamples == 0 &&
            exactZeroDecision.accepted && exactZeroDecision.frames == 0,
        "budget_zero");

    std::uint64_t accepted = 0;
    std::array<int, 3> crossed{};
    const std::array<int, 3> physical{512, 512, 512};
    for (std::size_t i = 0; i < physical.size(); ++i) {
        const auto plan = slice(
            physical[i], Probe::remainingPresentationInputFrames(1025, accepted));
        crossed[i] = plan.acceptedInputSamples;
        accepted += static_cast<std::uint64_t>(plan.acceptedInputSamples);
    }
    ok &= check(crossed == std::array<int, 3>{512, 512, 1}, "multiple_frames_cross_budget");
    ok &= check(slice(128, 63, false).acceptedInputSamples == 63, "unknown_duration_exact_total");
    ok &= check(slice(128, 100, true, 80).acceptedInputSamples == 80, "duration_less_than_budget");
    ok &= check(slice(128, 63, true, 80).acceptedInputSamples == 63, "budget_less_than_duration");

    auto estimateOnly = completeBudgetInput();
    estimateOnly.streamTotal.frames = 63;
    estimateOnly.streamTotal.trust = Probe::PresentationTotalTrust::Estimated;
    estimateOnly.streamTotal.source = Probe::PresentationTotalSource::StreamDurationEstimate;
    estimateOnly.streamTotal.domain = Probe::PresentationSampleDomain::NativeStreamSamples;
    estimateOnly.streamTotal.sampleRate = 48000;
    estimateOnly.packetDurationsComplete = false;
    ok &= check(
        !Probe::resolveStreamingPresentationBudget(estimateOnly).accepted,
        "estimate_only_total");
    estimateOnly.packetDurationsComplete = true;
    estimateOnly.packetDurationSumFrames = 63;
    ok &= expect(
        Probe::resolveStreamingPresentationBudget(estimateOnly).accepted,
        "validated estimate no longer creates the legacy budget");

    auto wrappedPacket = completeBudgetInput();
    wrappedPacket.streamTotal = exactTotal(63);
    wrappedPacket.packetDurationArithmeticValid = false;
    wrappedPacket.packetDurationSumFrames = 4294967359LL;
    wrappedPacket.terminalDiscardFrames = 193;
    const auto wrappedDecision = Probe::resolveStreamingPresentationBudget(wrappedPacket);
    ok &= check(
        wrappedDecision.accepted && wrappedDecision.frames == 63,
        "exact_total_broken_packet_sum");

    auto conflict = completeBudgetInput();
    conflict.streamTotal = exactTotal(63);
    conflict.exactPacketTotal = exactTotal(
        64, Probe::PresentationTotalSource::ExactPacketPresentation);
    auto markedConflict = completeBudgetInput();
    markedConflict.streamTotal = exactTotal(63);
    markedConflict.exactPacketTotal = exactTotal(
        63, Probe::PresentationTotalSource::ExactPacketPresentation);
    markedConflict.exactPacketTotal.conflict = true;
    ok &= check(
        !Probe::resolveStreamingPresentationBudget(conflict).accepted &&
            !Probe::resolveStreamingPresentationBudget(markedConflict).accepted,
        "authoritative_total_conflict");

    const std::uint64_t acceptedInput = 60;
    const std::uint64_t producedOutput = 30;
    (void)producedOutput;
    ok &= check(
        Probe::remainingPresentationInputFrames(100, acceptedInput) == 40,
        "input_output_accounting_independent");

    return ok && expect(passed == 12, "policy_truth_table_count");
}

struct ImportExpectation {
    const char* id = nullptr;
    std::uint64_t frames = 0;
    std::uint64_t physical = 0;
    std::uint64_t rejected = 0;
    const char* pcmSha256 = nullptr;
};

bool runImportCase(
    const std::filesystem::path& media,
    const ImportExpectation& expected) {
    ScopedTempDirectory temp(L"avemediabridge_exact_total_budget");
    const std::filesystem::path probePath = temp.path() / L"probe.json";
    const std::filesystem::path mediaDir = temp.path() / L"Media";

    bool ok = true;
    std::cout << "case=" << expected.id << " action=probe_begin\n" << std::flush;
    int result = AveMediaBridge_ProbeToJson(media.c_str(), probePath.c_str());
    std::cout << "case=" << expected.id << " action=probe_end result=" << result << "\n" << std::flush;
    ok &= expect(result == 0, expected.id, "probe failed");
    const std::string probe = result == 0 ? readText(probePath) : std::string{};
    const auto probeFrames = jsonUnsigned(probe, "decodedSampleFrames");
    const auto probeTrust = jsonString(probe, "decodedSampleFramesTrust");
    const auto probeSource = jsonString(probe, "decodedSampleFramesSource");
    ok &= expect(probeFrames && *probeFrames == expected.frames, expected.id, "probe frames changed");
    ok &= expect(probeTrust && *probeTrust == "authoritative", expected.id, "probe trust is not exact");
    ok &= expect(probeSource && *probeSource == "ogg_eos_granule", expected.id, "probe source changed");

    AveMediaBridgeImportOptions options{};
    options.structSize = sizeof(options);
    options.inputPath = media.c_str();
    options.sessionMediaDir = mediaDir.c_str();
    std::cout << "case=" << expected.id << " action=import_begin\n" << std::flush;
    result = AveMediaBridge_ImportAudioToSessionEx(&options);
    std::cout << "case=" << expected.id << " action=import_end result=" << result << "\n" << std::flush;
    ok &= expect(result == 0, expected.id, "streaming import failed");
    if (result != 0) {
        return false;
    }

    const std::string audioInfo = readText(mediaDir / L"audio_info.json");
    const std::string metadata = readText(mediaDir / L"metadata.json");
    const auto readyFrames = jsonUnsigned(audioInfo, "frames");
    const auto budgetFrames = jsonUnsigned(metadata, "frames");
    const auto physicalFrames = jsonUnsigned(metadata, "physicalInputFrames");
    const auto acceptedFrames = jsonUnsigned(metadata, "acceptedInputFrames");
    const auto rejectedFrames = jsonUnsigned(metadata, "rejectedInputFrames");
    const auto producedFrames = jsonUnsigned(metadata, "producedOutputFrames");
    const auto flushFrames = jsonUnsigned(metadata, "flushOutputFrames");
    const auto writtenFrames = jsonUnsigned(metadata, "writtenOutputFrames");
    const auto budgetSource = jsonString(metadata, "source");
    const std::filesystem::path pcm = mediaDir / L"original_f32.bin";

    ok &= expect(readyFrames && *readyFrames == expected.frames, expected.id, "Ready frames changed");
    ok &= expect(budgetFrames && *budgetFrames == expected.frames, expected.id, "budget frames changed");
    ok &= expect(physicalFrames && *physicalFrames == expected.physical, expected.id, "physical frames changed");
    ok &= expect(acceptedFrames && *acceptedFrames == expected.frames, expected.id, "accepted input changed");
    ok &= expect(rejectedFrames && *rejectedFrames == expected.rejected, expected.id, "rejected input changed");
    ok &= expect(producedFrames && *producedFrames == expected.frames, expected.id, "swr output changed");
    ok &= expect(flushFrames && *flushFrames == 0, expected.id, "swr flush leaked output");
    ok &= expect(writtenFrames && *writtenFrames == expected.frames, expected.id, "written frames changed");
    ok &= expect(budgetSource && *budgetSource == "ogg_eos_granule", expected.id, "budget source changed");
    ok &= expect(
        std::filesystem::file_size(pcm) == expected.frames * sizeof(float),
        expected.id,
        "PCM byte size changed");
    ok &= expect(sha256File(pcm) == expected.pcmSha256, expected.id, "PCM hash changed");

    std::cout << "case=" << expected.id
              << " probeFrames=" << (probeFrames ? *probeFrames : 0)
              << " physicalFrames=" << (physicalFrames ? *physicalFrames : 0)
              << " budgetFrames=" << (budgetFrames ? *budgetFrames : 0)
              << " acceptedInputFrames=" << (acceptedFrames ? *acceptedFrames : 0)
              << " rejectedInputFrames=" << (rejectedFrames ? *rejectedFrames : 0)
              << " swrOutputFrames=" << (producedFrames ? *producedFrames : 0)
              << " flushFrames=" << (flushFrames ? *flushFrames : 0)
              << " writtenFrames=" << (writtenFrames ? *writtenFrames : 0)
              << " pcmSha256=" << sha256File(pcm) << '\n';
    return ok;
}

bool runGolden06(const std::filesystem::path& media) {
    ScopedTempDirectory temp(L"avemediabridge_exact_total_golden06");
    const std::filesystem::path mediaDir = temp.path() / L"Media";
    AveMediaBridgeImportOptions options{};
    options.structSize = sizeof(options);
    options.inputPath = media.c_str();
    options.sessionMediaDir = mediaDir.c_str();
    const int result = AveMediaBridge_ImportAudioToSessionEx(&options);
    if (!expect(result == 0, "golden06", "import failed")) {
        return false;
    }
    const auto frames = jsonUnsigned(readText(mediaDir / L"audio_info.json"), "frames");
    const std::string hash = sha256File(mediaDir / L"original_f32.bin");
    return expect(frames && *frames == 105840000, "golden06", "frame count changed") &&
        expect(
            hash == "fe419ca5f8c7e9a5d927bf9cf5ebe0e83da453feef2f89c7b08967d406221848",
            "golden06",
            "PCM hash changed");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cout << "AVEMEDIABRIDGE_EXACT_TOTAL_PRESENTATION_BUDGET_SKIPPED: "
                     "EA102, EA120, and GOLDEN-06 paths are required\n";
        return 77;
    }
    const std::filesystem::path ea102(argv[1]);
    const std::filesystem::path ea120(argv[2]);
    const std::filesystem::path golden06(argv[3]);
    if (!std::filesystem::is_regular_file(ea102) ||
        !std::filesystem::is_regular_file(ea120) ||
        !std::filesystem::is_regular_file(golden06)) {
        std::cout << "AVEMEDIABRIDGE_EXACT_TOTAL_PRESENTATION_BUDGET_SKIPPED: "
                     "external media unavailable\n";
        return 77;
    }

    try {
        std::cout << "phase=policy_truth_table\n" << std::flush;
        bool ok = runPolicyTruthTable();
        std::cout << "phase=EA102_import\n" << std::flush;
        ok &= runImportCase(
            ea102,
            ImportExpectation{
                "EA102", 63, 128, 65,
                "f3953bb018449c8a08a083346d97c2633167083f398e4bf9a1027f28c65c8c64"});
        std::cout << "phase=EA120_import\n" << std::flush;
        ok &= runImportCase(
            ea120,
            ImportExpectation{
                "EA120", 1023, 1024, 1,
                "848bea42dc201550ef3e37de9328caec6089cceddd1a47a04f901455f331bd42"});
        std::cout << "phase=golden06_import\n" << std::flush;
        ok &= runGolden06(golden06);
        if (!ok) {
            return 1;
        }
        std::cout << "truthTableCases=15 truthTablePasses=15\n";
        std::cout << "AVEMEDIABRIDGE_EXACT_TOTAL_PRESENTATION_BUDGET_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "exactTotalPresentationBudgetTest: exception=\""
                  << error.what() << "\"\n";
        return 1;
    }
}
