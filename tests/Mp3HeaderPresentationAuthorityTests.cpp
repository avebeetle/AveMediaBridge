#include "include/AveMediaBridge/AveMediaBridgeApi.hpp"
#include "src/Probe/Mp3HeaderPresentation.hpp"

#include <algorithm>
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
    std::cerr << "mp3HeaderPresentationAuthorityTest: case=\"" << name << "\"";
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

std::optional<bool> jsonBool(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    const std::size_t keyAt = json.find(token);
    const std::size_t colon = keyAt == std::string::npos
        ? std::string::npos
        : json.find(':', keyAt + token.size());
    const std::size_t value = colon == std::string::npos
        ? std::string::npos
        : json.find_first_not_of(" \t\r\n", colon + 1);
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

struct ExactCase {
    const wchar_t* relativePath;
    const char* id;
    std::uint64_t frames;
    std::uint64_t samplesPerFrame;
    Probe::Mp3HeaderType type;
};

bool runExactParserMatrix(
    const std::filesystem::path& frozenRoot,
    const std::filesystem::path& fixtureRoot,
    const std::filesystem::path& realMp3) {
    const std::array<std::pair<std::filesystem::path, ExactCase>, 15> cases{
        std::pair{realMp3, ExactCase{L"", "real_40_minute_info", 105840000, 1152, Probe::Mp3HeaderType::Info}},
        std::pair{realMp3.parent_path(), ExactCase{L"output.mp3", "real_output_mp3", 115200000, 1152, Probe::Mp3HeaderType::Info}},
        std::pair{frozenRoot, ExactCase{L"exact_authority/valid/mp3/EA029_mp3_44100_11521.mp3", "mpeg1_frozen", 11567, 1152, Probe::Mp3HeaderType::Info}},
        std::pair{frozenRoot, ExactCase{L"exact_authority/valid/mp3/EA093_mp3_44100_26460047.mp3", "long_mpeg1_frozen", 26460047, 1152, Probe::Mp3HeaderType::Info}},
        std::pair{frozenRoot, ExactCase{L"exact_authority/valid/mp3/EA100_mp3_48000_2.mp3", "very_short_mpeg1_frozen", 47, 1152, Probe::Mp3HeaderType::Info}},
        std::pair{frozenRoot, ExactCase{L"exact_authority/valid/mp3/EA126_mp3_16000_115153.mp3", "mpeg2_frozen", 115153, 576, Probe::Mp3HeaderType::Info}},
        std::pair{frozenRoot, ExactCase{L"exact_authority/valid/mp3/EA139_mp3_8000_7999.mp3", "mpeg25_frozen", 7999, 576, Probe::Mp3HeaderType::Info}},
        std::pair{fixtureRoot, ExactCase{L"cbr_info.mp3", "cbr_info", 103415, 1152, Probe::Mp3HeaderType::Info}},
        std::pair{fixtureRoot, ExactCase{L"vbr_xing.mp3", "vbr_xing", 103415, 1152, Probe::Mp3HeaderType::Xing}},
        std::pair{fixtureRoot, ExactCase{L"mpeg2_info.mp3", "mpeg2_info", 21392, 576, Probe::Mp3HeaderType::Info}},
        std::pair{fixtureRoot, ExactCase{L"mpeg25_info.mp3", "mpeg25_info", 10696, 576, Probe::Mp3HeaderType::Info}},
        std::pair{fixtureRoot, ExactCase{L"non_frame_aligned.mp3", "non_frame_aligned", 100003, 1152, Probe::Mp3HeaderType::Xing}},
        std::pair{fixtureRoot, ExactCase{L"id3v1_tail.mp3", "id3v1_tail", 103415, 1152, Probe::Mp3HeaderType::Info}},
        std::pair{fixtureRoot, ExactCase{L"apev2_tail.mp3", "apev2_tail", 103415, 1152, Probe::Mp3HeaderType::Info}},
        std::pair{fixtureRoot, ExactCase{L"large_id3v2_padding.mp3", "large_id3v2", 103415, 1152, Probe::Mp3HeaderType::Info}},
    };

    bool ok = true;
    for (const auto& [root, item] : cases) {
        const std::filesystem::path path = item.relativePath[0] == L'\0'
            ? root
            : root / item.relativePath;
        const Probe::Mp3HeaderPresentationResult result =
            Probe::probeMp3HeaderPresentation(path.u8string());
        ok &= expect(result.exact(), item.id, result.reason);
        ok &= expect(result.presentationFrames == item.frames, item.id, "presentation frames");
        ok &= expect(result.samplesPerFrame == item.samplesPerFrame, item.id, "samples per frame");
        ok &= expect(result.headerType == item.type, item.id, "header type");
        ok &= expect(
            result.trust == Probe::PresentationTotalTrust::SampleExact &&
                result.source == Probe::PresentationTotalSource::Mp3ValidatedHeaderPresentation &&
                result.domain == Probe::PresentationSampleDomain::NativeStreamSamples,
            item.id, "typed authority");
        ok &= expect(result.totalActualReadBytes <= result.hardReadBudgetBytes,
            item.id, "hard budget");
        ok &= expect(result.maximumBudgetOverrunBytes == 0, item.id, "budget overrun");
        ok &= expect(
            result.validation.leadingTagValidated &&
                result.validation.trailingTagsValidated &&
                result.validation.firstFrameValidated &&
                result.validation.secondFrameValidated &&
                result.validation.byteRangeValidated &&
                result.validation.encoderProfileValidated &&
                result.validation.tagCrcValidated &&
                result.validation.gaplessFieldsValidated &&
                result.validation.checkedArithmetic,
            item.id, "validation flags");
    }
    return ok;
}

bool runSelectedStreamDomainValidation(const std::filesystem::path& fixtureRoot) {
    const Probe::Mp3HeaderPresentationResult result =
        Probe::probeMp3HeaderPresentation((fixtureRoot / L"cbr_info.mp3").u8string());
    AVCodecParameters codecpar{};
    codecpar.codec_type = AVMEDIA_TYPE_AUDIO;
    codecpar.codec_id = AV_CODEC_ID_MP3;
    codecpar.sample_rate = result.sampleRate;
    codecpar.ch_layout.nb_channels = result.channels;
    AVStream stream{};
    stream.codecpar = &codecpar;

    bool ok = expect(
        Probe::mp3HeaderPresentationMatchesStream(result, &stream),
        "selected_stream_domain", "matching stream rejected");
    ++codecpar.sample_rate;
    ok &= expect(
        !Probe::mp3HeaderPresentationMatchesStream(result, &stream),
        "selected_stream_domain", "sample-rate conflict accepted");
    --codecpar.sample_rate;
    ++codecpar.ch_layout.nb_channels;
    ok &= expect(
        !Probe::mp3HeaderPresentationMatchesStream(result, &stream),
        "selected_stream_domain", "channel conflict accepted");
    return ok;
}

bool runAdversarialMatrix(
    const std::filesystem::path& fixtureRoot,
    std::size_t& falseExactCount,
    std::array<std::size_t, 5>& statusCounts) {
    const std::array<const wchar_t*, 18> files{
        L"corrupt_xing_small.mp3", L"corrupt_xing_large.mp3",
        L"zero_xing_count.mp3", L"xing_bytes_too_large.mp3",
        L"truncated_xing_header.mp3", L"fake_xing_payload.mp3",
        L"invalid_lame.mp3", L"missing_lame.mp3",
        L"delay_padding_exceeds_total.mp3", L"conflicting_xing_vbri.mp3",
        L"concatenated.mp3", L"trailing_garbage.mp3",
        L"truncated_tail.mp3", L"truncated_head.mp3",
        L"integer_overflow_candidate.mp3", L"checksum_valid_info_plus_one.mp3",
        L"no_xing.mp3", L"vbri_physical_only.mp3"};
    auto observeStatus = [&](Probe::Mp3HeaderPresentationStatus status) {
        ++statusCounts[static_cast<std::size_t>(status)];
    };
    bool ok = true;
    for (const wchar_t* name : files) {
        const Probe::Mp3HeaderPresentationResult result =
            Probe::probeMp3HeaderPresentation((fixtureRoot / name).u8string());
        observeStatus(result.status);
        falseExactCount += result.exact() ? 1U : 0U;
        ok &= expect(!result.exact(), std::filesystem::path(name).u8string(), "false Exact");
        ok &= expect(result.maximumBudgetOverrunBytes == 0,
            std::filesystem::path(name).u8string(), "budget overrun");
    }

    const Probe::Mp3HeaderPresentationResult vbri =
        Probe::probeMp3HeaderPresentation(
            (fixtureRoot / L"vbri_physical_only.mp3").u8string());
    ok &= expect(
        !vbri.exact() && vbri.headerType == Probe::Mp3HeaderType::Vbri &&
            vbri.physicalFrameCount == 91 &&
            vbri.samplesPerFrame == 1152 &&
            vbri.physicalSampleTotal == 104832 &&
            vbri.validation.byteRangeValidated &&
            vbri.validation.checkedArithmetic,
        "vbri_physical_only", "validated physical total must not become presentation Exact");

    const std::filesystem::path valid = fixtureRoot / L"cbr_info.mp3";
    for (const auto& item : std::array<std::pair<const char*, Probe::Mp3HeaderPresentationProbeOptions>, 4>{
             std::pair{"budget_exhaustion", [] { Probe::Mp3HeaderPresentationProbeOptions value; value.hardReadBudgetBytes = 64; return value; }()},
             {"non_seekable", [] { Probe::Mp3HeaderPresentationProbeOptions value; value.forceInputNonSeekable = true; return value; }()},
             {"seek_error", [] { Probe::Mp3HeaderPresentationProbeOptions value; value.forceSeekFailure = true; return value; }()},
             {"read_error", [] { Probe::Mp3HeaderPresentationProbeOptions value; value.forceReadErrorAfterBytes = 16; return value; }()}}) {
        const Probe::Mp3HeaderPresentationResult result =
            Probe::probeMp3HeaderPresentation(valid.u8string(), item.second);
        observeStatus(result.status);
        falseExactCount += result.exact() ? 1U : 0U;
        ok &= expect(!result.exact(), item.first, "forced failure false Exact");
        ok &= expect(result.maximumBudgetOverrunBytes == 0, item.first, "forced failure overrun");
    }
    return ok;
}

bool runProbeIntegration(
    const std::filesystem::path& realMp3,
    const std::filesystem::path& noXing,
    const std::filesystem::path& mp4Mp3) {
    ScopedTempDirectory temp(L"avemediabridge_mp3_header_probe");
    bool ok = true;
    auto probe = [&](const std::filesystem::path& media, const wchar_t* name) {
        const std::filesystem::path output = temp.path() / name;
        const int code = AveMediaBridge_ProbeToJson(media.c_str(), output.c_str());
        ok &= expect(code == 0, output.filename().u8string(), "probe failed");
        return code == 0 ? readText(output) : std::string{};
    };

    const std::string real = probe(realMp3, L"real.json");
    ok &= expect(jsonUnsigned(real, "decodedSampleFrames") == 105840000,
        "real_probe", "Loading frames");
    ok &= expect(jsonString(real, "decodedSampleFramesSource") ==
        "mp3_validated_header_presentation", "real_probe", "typed source");
    ok &= expect(jsonString(real, "mp3HeaderPresentationStatus") == "exact",
        "real_probe", "header status");
    ok &= expect(jsonBool(real, "mp3HeaderFullScanSkipped") == true,
        "real_probe", "full scan not skipped");
    ok &= expect(jsonUnsigned(real, "gaplessAudioPacketsScanned") == 0,
        "real_probe", "legacy scan entered");
    ok &= expect(jsonUnsigned(real, "mp3HeaderMaximumBudgetOverrunBytes") == 0,
        "real_probe", "budget overrun");

    const std::string fallback = probe(noXing, L"fallback.json");
    ok &= expect(jsonString(fallback, "mp3HeaderPresentationStatus") == "unavailable",
        "no_xing", "header fallback status");
    ok &= expect(jsonUnsigned(fallback, "decodedSampleFrames") == 104832,
        "no_xing", "same-pass exact total");
    ok &= expect(jsonString(fallback, "decodedSampleFramesSource") ==
        "exact_packet_presentation", "no_xing", "fallback typed source");
    ok &= expect(jsonUnsigned(fallback, "gaplessAudioPacketsScanned") == 91,
        "no_xing", "fallback traversal count");

    const std::string mp4 = probe(mp4Mp3, L"mp4.json");
    ok &= expect(jsonString(mp4, "mp3HeaderPresentationStatus") == "not_eligible",
        "mp4_mp3", "standalone parser entered");
    ok &= expect(jsonUnsigned(mp4, "mp3HeaderActualReadBytes") == 0,
        "mp4_mp3", "standalone parser read bytes");
    return ok;
}

bool runShortDecodeParity(
    const std::filesystem::path& media,
    std::uint64_t frames,
    const char* expectedHash,
    const char* id) {
    ScopedTempDirectory temp(L"avemediabridge_mp3_header_decode");
    const std::filesystem::path session = temp.path() / L"Media";
    AveMediaBridgeImportOptions options{};
    options.structSize = sizeof(options);
    options.inputPath = media.c_str();
    options.sessionMediaDir = session.c_str();
    bool ok = expect(AveMediaBridge_ImportAudioToSessionEx(&options) == 0, id, "import failed");
    if (!ok) {
        return false;
    }
    const std::string audioInfo = readText(session / L"audio_info.json");
    const std::string metadata = readText(session / L"metadata.json");
    ok &= expect(jsonUnsigned(audioInfo, "frames") == frames, id, "Ready frames");
    ok &= expect(sha256File(session / L"original_f32.bin") == expectedHash,
        id, "PCM hash changed");
    if (std::string(id) == "EA029") {
        ok &= expect(jsonString(metadata, "source") ==
            "mp3_validated_header_presentation", id, "streaming budget source");
        ok &= expect(jsonBool(metadata, "packetScanExecuted") == false,
            id, "streaming budget full scan entered");
    }
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        std::cout << "AVEMEDIABRIDGE_MP3_HEADER_AUTHORITY_SKIPPED: "
                     "frozen root, fixture root, real MP3, no-Xing MP3, and MP4/MP3 are required\n";
        return 77;
    }

    const std::filesystem::path frozenRoot = std::filesystem::u8path(argv[1]);
    const std::filesystem::path fixtureRoot = std::filesystem::u8path(argv[2]);
    const std::filesystem::path realMp3 = std::filesystem::u8path(argv[3]);
    const std::filesystem::path noXing = std::filesystem::u8path(argv[4]);
    const std::filesystem::path mp4Mp3 = std::filesystem::u8path(argv[5]);
    if (!std::filesystem::is_directory(frozenRoot) ||
        !std::filesystem::is_directory(fixtureRoot) ||
        !std::filesystem::is_regular_file(realMp3) ||
        !std::filesystem::is_regular_file(noXing) ||
        !std::filesystem::is_regular_file(mp4Mp3)) {
        std::cout << "AVEMEDIABRIDGE_MP3_HEADER_AUTHORITY_SKIPPED: required media unavailable\n";
        return 77;
    }

    bool ok = true;
    std::size_t falseExactCount = 0;
    std::array<std::size_t, 5> statusCounts{};
    std::vector<double> parserWarmMs;
    Probe::Mp3HeaderPresentationResult measuredHeader;
    for (int iteration = 0; iteration < 11; ++iteration) {
        const auto started = std::chrono::steady_clock::now();
        measuredHeader = Probe::probeMp3HeaderPresentation(realMp3.u8string());
        const double elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        if (iteration > 0) {
            parserWarmMs.push_back(elapsedMs);
        }
    }
    std::sort(parserWarmMs.begin(), parserWarmMs.end());
    const double parserWarmMedianMs =
        (parserWarmMs[4] + parserWarmMs[5]) / 2.0;
    ok &= expect(measuredHeader.exact(), "real_parser_timing", measuredHeader.reason);
    ok &= runExactParserMatrix(frozenRoot, fixtureRoot, realMp3);
    ok &= runSelectedStreamDomainValidation(fixtureRoot);
    ok &= runAdversarialMatrix(fixtureRoot, falseExactCount, statusCounts);
    ok &= runProbeIntegration(realMp3, noXing, mp4Mp3);
    ok &= runShortDecodeParity(
        frozenRoot / L"exact_authority/valid/mp3/EA029_mp3_44100_11521.mp3",
        11567,
        "ed8d2781a836ff86e3b9b1b3847edb950dedff634e2ce23ebe0f628aaf8f24ad",
        "EA029");
    ok &= runShortDecodeParity(
        noXing,
        104832,
        "6b938925035a75ad2def72f707c537de67b0d81c6aca1594ad980219f17b1510",
        "GEN_NO_XING");
    ok &= expect(falseExactCount == 0, "adversarial_false_exact_count");

    if (!ok) {
        return 1;
    }
    std::cout << "AVEMEDIABRIDGE_MP3_HEADER_AUTHORITY_OK "
              << "falseExactCount=" << falseExactCount
              << " maximumBudgetOverrunBytes=0"
              << " unavailableCount="
              << statusCounts[static_cast<std::size_t>(Probe::Mp3HeaderPresentationStatus::Unavailable)]
              << " ioBudgetExceededCount="
              << statusCounts[static_cast<std::size_t>(Probe::Mp3HeaderPresentationStatus::IoBudgetExceeded)]
              << " conflictCount="
              << statusCounts[static_cast<std::size_t>(Probe::Mp3HeaderPresentationStatus::Conflict)]
              << " invalidCount="
              << statusCounts[static_cast<std::size_t>(Probe::Mp3HeaderPresentationStatus::InvalidMedia)]
              << " realHeaderBytes=" << measuredHeader.totalActualReadBytes
              << " realHeaderUniqueBytes=" << measuredHeader.uniqueBytesRead
              << " parserWarmMedianMs=" << parserWarmMedianMs
              << '\n';
    return 0;
}
