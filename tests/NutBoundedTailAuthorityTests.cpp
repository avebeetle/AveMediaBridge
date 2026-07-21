#include "include/AveMediaBridge/AveMediaBridgeApi.hpp"
#include "src/Probe/NutBoundedTailAuthority.hpp"

#include <array>
#include <chrono>
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
    std::cerr << "nutBoundedTailAuthorityTest: case=\"" << name << "\"";
    if (!detail.empty()) {
        std::cerr << " detail=\"" << detail << "\"";
    }
    std::cerr << '\n';
    return false;
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

struct RealCase {
    const char* id;
    std::uint64_t frames;
    std::uint64_t maximumReadBytes;
    int channels;
    const char* pcmSha256;
};

struct RealCaseResult {
    std::uint64_t bytesRead = 0;
    std::string pcmSha256;
};

RealCaseResult runRealCase(
    const std::filesystem::path& media,
    const RealCase& expected,
    bool& ok) {
    ScopedTempDirectory temp(L"avemediabridge_nut_bounded_tail");
    const std::filesystem::path probePath = temp.path() / L"probe.json";
    const std::filesystem::path reopenPath = temp.path() / L"reopen.json";
    const std::filesystem::path mediaDir = temp.path() / L"Media";

    int result = AveMediaBridge_ProbeToJson(media.c_str(), probePath.c_str());
    ok &= expect(result == 0, expected.id, "probe failed");
    const std::string probe = result == 0 ? readText(probePath) : std::string{};
    const auto loadingFrames = jsonUnsigned(probe, "decodedSampleFrames");
    const auto source = jsonString(probe, "decodedSampleFramesSource");
    const auto status = jsonString(probe, "nutBoundedTailStatus");
    const auto bytesRead = jsonUnsigned(probe, "nutBoundedTailActualReadBytes");
    const auto overrun = jsonUnsigned(probe, "nutBoundedTailMaximumBudgetOverrunBytes");
    ok &= expect(loadingFrames && *loadingFrames == expected.frames, expected.id, "Loading frames");
    ok &= expect(source && *source == "nut_bounded_tail_selected_stream_end",
        expected.id, "typed source");
    ok &= expect(status && *status == "exact", expected.id, "bounded status");
    ok &= expect(bytesRead && *bytesRead <= expected.maximumReadBytes,
        expected.id, "read budget");
    ok &= expect(overrun && *overrun == 0, expected.id, "budget overrun");

    AveMediaBridgeImportOptions options{};
    options.structSize = sizeof(options);
    options.inputPath = media.c_str();
    options.sessionMediaDir = mediaDir.c_str();
    result = AveMediaBridge_ImportAudioToSessionEx(&options);
    ok &= expect(result == 0, expected.id, "ordinary import failed");
    if (result != 0) {
        return {};
    }
    const std::string audioInfo = readText(mediaDir / L"audio_info.json");
    const auto readyFrames = jsonUnsigned(audioInfo, "frames");
    const std::filesystem::path pcm = mediaDir / L"original_f32.bin";
    const std::string pcmHash = sha256File(pcm);
    ok &= expect(readyFrames && *readyFrames == expected.frames, expected.id, "Ready frames");
    ok &= expect(std::filesystem::file_size(pcm) ==
        expected.frames * static_cast<std::uint64_t>(expected.channels) * sizeof(float),
        expected.id, "PCM bytes");
    ok &= expect(pcmHash == expected.pcmSha256, expected.id, "PCM hash");

    result = AveMediaBridge_ProbeToJson(media.c_str(), reopenPath.c_str());
    const std::string reopen = result == 0 ? readText(reopenPath) : std::string{};
    const auto reopenFrames = jsonUnsigned(reopen, "decodedSampleFrames");
    ok &= expect(result == 0 && reopenFrames && *reopenFrames == expected.frames,
        expected.id, "Reopen frames");

    std::cout << "case=" << expected.id
              << " loadingFrames=" << (loadingFrames ? *loadingFrames : 0)
              << " readyFrames=" << (readyFrames ? *readyFrames : 0)
              << " reopenFrames=" << (reopenFrames ? *reopenFrames : 0)
              << " actualReadBytes=" << (bytesRead ? *bytesRead : 0)
              << " pcmSha256=" << pcmHash << '\n';
    return {bytesRead.value_or(0), pcmHash};
}

bool statusIsSafeFailure(Probe::NutBoundedTailStatus status) {
    return status == Probe::NutBoundedTailStatus::Unavailable ||
        status == Probe::NutBoundedTailStatus::IoBudgetExceeded ||
        status == Probe::NutBoundedTailStatus::InvalidMedia ||
        status == Probe::NutBoundedTailStatus::Conflict;
}

bool checkFailure(
    const std::filesystem::path& media,
    const char* name,
    Probe::NutBoundedTailTestHooks hooks,
    std::uint64_t budget,
    bool requireBudgetExceeded,
    std::size_t& falseExactCount) {
    Probe::NutBoundedTailProbeOptions options;
    options.hardReadBudgetBytes = budget;
    options.testHooks = &hooks;
    const Probe::NutBoundedTailProbeResult result =
        Probe::probeNutBoundedTailAuthority(media.u8string(), 0, options);
    falseExactCount += result.exact() ? 1U : 0U;
    return expect(statusIsSafeFailure(result.status), name, "unsafe status") &&
        expect(!result.exact(), name, "false Exact") &&
        expect(result.maximumBudgetOverrunBytes == 0, name, "budget overrun") &&
        expect(result.totalActualReadBytes <= budget, name, "actual reads exceed budget") &&
        expect(!requireBudgetExceeded ||
            result.status == Probe::NutBoundedTailStatus::IoBudgetExceeded,
            name, "budget exhaustion not observed");
}

bool runFailureMatrix(
    const std::filesystem::path& target,
    const std::filesystem::path& fixtureRoot,
    std::size_t& falseExactCount) {
    bool ok = true;
    Probe::NutBoundedTailTestHooks hooks;
    hooks.overrideSeekTarget = true;
    hooks.seekTarget = 0;
    ok &= checkFailure(target, "stale_early", hooks, 64 * 1024, true, falseExactCount);

    hooks = {};
    hooks.overrideSeekTarget = true;
    hooks.seekTarget = (std::numeric_limits<std::int64_t>::max)() / 2;
    hooks.suppressSelectedPackets = true;
    ok &= checkFailure(target, "stale_late", hooks,
        Probe::kNutBoundedTailReadBudgetBytes, false, falseExactCount);
    hooks = {};
    hooks.forceNoIndex = true;
    ok &= checkFailure(target, "no_index", hooks,
        Probe::kNutBoundedTailReadBudgetBytes, false, falseExactCount);
    hooks = {};
    hooks.forceNonSeekable = true;
    ok &= checkFailure(target, "pipe_style", hooks,
        Probe::kNutBoundedTailReadBudgetBytes, false, falseExactCount);
    ok &= checkFailure(target, "non_seekable", hooks,
        Probe::kNutBoundedTailReadBudgetBytes, false, falseExactCount);
    hooks = {};
    hooks.syntheticEofAfterBytes = 96 * 1024;
    ok &= checkFailure(target, "truncated_tail", hooks,
        Probe::kNutBoundedTailReadBudgetBytes, false, falseExactCount);
    hooks = {};
    hooks.forceMissingDuration = true;
    ok &= checkFailure(target, "missing_duration", hooks,
        Probe::kNutBoundedTailReadBudgetBytes, false, falseExactCount);
    hooks = {};
    hooks.forceMisalignedPayload = true;
    ok &= checkFailure(target, "misaligned_payload", hooks,
        Probe::kNutBoundedTailReadBudgetBytes, false, falseExactCount);
    hooks = {};
    hooks.suppressSelectedPackets = true;
    ok &= checkFailure(target, "selected_stream_absent", hooks,
        Probe::kNutBoundedTailReadBudgetBytes, false, falseExactCount);
    hooks = {};
    hooks.forceSeekFailure = true;
    ok &= checkFailure(target, "seek_failure", hooks,
        Probe::kNutBoundedTailReadBudgetBytes, false, falseExactCount);
    hooks = {};
    hooks.readErrorAfterBytes = 96 * 1024;
    ok &= checkFailure(target, "read_error", hooks,
        Probe::kNutBoundedTailReadBudgetBytes, false, falseExactCount);

    const std::array<const wchar_t*, 5> actualFailureFixtures{
        L"stale_index_early.nut", L"stale_index_late.nut", L"no_index.nut",
        L"pipe_style.nut", L"truncated_tail.nut"};
    for (const wchar_t* filename : actualFailureFixtures) {
        const std::filesystem::path path = fixtureRoot / filename;
        if (!std::filesystem::is_regular_file(path)) {
            continue;
        }
        Probe::NutBoundedTailProbeOptions options;
        const auto result = Probe::probeNutBoundedTailAuthority(path.u8string(), 0, options);
        falseExactCount += result.exact() ? 1U : 0U;
        ok &= expect(statusIsSafeFailure(result.status), path.filename().u8string(), "fixture status");
        ok &= expect(!result.exact(), path.filename().u8string(), "fixture false Exact");
        ok &= expect(result.maximumBudgetOverrunBytes == 0,
            path.filename().u8string(), "fixture budget overrun");
    }
    return ok;
}

bool runMultistreamMatrix(const std::filesystem::path& root) {
    struct Case {
        const wchar_t* file;
        int streamIndex;
        std::uint64_t frames;
        const char* name;
    };
    const std::array<Case, 8> cases{
        Case{L"two_audio.nut", 0, 240123, "two_audio_selected_0"},
        Case{L"two_audio.nut", 1, 308777, "two_audio_selected_1"},
        Case{L"audio_video.nut", 1, 264123, "audio_video"},
        Case{L"video_ends_later.nut", 1, 192123, "selected_audio_ends_first"},
        Case{L"audio_ends_later.nut", 1, 336123, "selected_audio_ends_last"},
        Case{L"other_audio_later.nut", 0, 144123, "other_audio_ends_later_selected"},
        Case{L"other_audio_later.nut", 1, 384321, "other_audio_ends_later_other"},
        Case{L"nonzero_start.nut", 0, 96123, "nonzero_stream_start"},
    };
    bool ok = true;
    for (const Case& item : cases) {
        const std::filesystem::path path = root / item.file;
        if (!std::filesystem::is_regular_file(path)) {
            return expect(false, "multistream_fixture_missing", path.u8string());
        }
        Probe::NutBoundedTailProbeOptions options;
        options.hardReadBudgetBytes = 1024 * 1024;
        const auto result = Probe::probeNutBoundedTailAuthority(
            path.u8string(), item.streamIndex, options);
        ok &= expect(result.exact() && result.presentationFrames == item.frames,
            item.name, result.reason);
        ok &= expect(result.maximumBudgetOverrunBytes == 0, item.name, "budget overrun");
    }
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cout << "AVEMEDIABRIDGE_NUT_BOUNDED_TAIL_AUTHORITY_SKIPPED: "
                     "EA021, EA119, EA136, and research fixture root are required\n";
        return 77;
    }
    for (int index = 1; index <= 3; ++index) {
        if (!std::filesystem::is_regular_file(std::filesystem::path(argv[index]))) {
            std::cout << "AVEMEDIABRIDGE_NUT_BOUNDED_TAIL_AUTHORITY_SKIPPED: "
                         "frozen target unavailable\n";
            return 77;
        }
    }
    const std::filesystem::path fixtureRoot(argv[4]);
    if (!std::filesystem::is_directory(fixtureRoot)) {
        std::cout << "AVEMEDIABRIDGE_NUT_BOUNDED_TAIL_AUTHORITY_SKIPPED: "
                     "research fixtures unavailable\n";
        return 77;
    }

    try {
        bool ok = true;
        const std::array<RealCase, 3> cases{
            RealCase{"EA021", 257, 64 * 1024, 6,
                "47fa3f9eee9c390d7bc4f5b9edbf86b3270a0fe1b1245c80e4e9f8361e08c558"},
            RealCase{"EA119", 191999, Probe::kNutBoundedTailReadBudgetBytes, 1,
                "27522b53f8673dfc17245d61487caf5658411711894de1c2cc0e8b7197ecdae3"},
            RealCase{"EA136", 1058537, Probe::kNutBoundedTailReadBudgetBytes, 2,
                "3e6dfdee900d9c8197d37c17360be4320397d0823a20dbdff35101649ead4180"},
        };
        std::array<RealCaseResult, 3> results{};
        for (int index = 0; index < 3; ++index) {
            results[index] = runRealCase(
                std::filesystem::path(argv[index + 1]), cases[index], ok);
        }

        Probe::NutBoundedTailProbeOptions warmup;
        (void)Probe::probeNutBoundedTailAuthority(
            std::filesystem::path(argv[2]).u8string(), 0, warmup);
        DWORD handlesBefore = 0;
        GetProcessHandleCount(GetCurrentProcess(), &handlesBefore);
        std::size_t falseExactCount = 0;
        ok &= runFailureMatrix(
            std::filesystem::path(argv[3]), fixtureRoot, falseExactCount);
        ok &= runMultistreamMatrix(fixtureRoot);
        DWORD handlesAfter = 0;
        GetProcessHandleCount(GetCurrentProcess(), &handlesAfter);
        ok &= expect(handlesAfter == handlesBefore, "secondary_context_handle_leak");
        ok &= expect(falseExactCount == 0, "no_false_exact");

        bool postFailureImportOk = true;
        const RealCaseResult postFailure = runRealCase(
            std::filesystem::path(argv[2]), cases[1], postFailureImportOk);
        ok &= postFailureImportOk;
        ok &= expect(postFailure.pcmSha256 == results[1].pcmSha256,
            "ordinary_decode_after_failure", "PCM hash changed");

        AVCodecParameters compressed{};
        compressed.codec_type = AVMEDIA_TYPE_AUDIO;
        compressed.codec_id = AV_CODEC_ID_AAC;
        compressed.sample_rate = 48000;
        av_channel_layout_default(&compressed.ch_layout, 2);
        ok &= expect(!Probe::deriveUncompressedPcmPacketLayout(&compressed).eligible,
            "compressed_payload_rejected");
        av_channel_layout_uninit(&compressed.ch_layout);

        if (!ok) {
            return 1;
        }
        std::cout << "failureCases=16 falseExactCount=" << falseExactCount
                  << " multistreamCases=8 maximumBudgetOverrunBytes=0\n";
        std::cout << "AVEMEDIABRIDGE_NUT_BOUNDED_TAIL_AUTHORITY_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "nutBoundedTailAuthorityTest: exception=\""
                  << error.what() << "\"\n";
        return 1;
    }
}
