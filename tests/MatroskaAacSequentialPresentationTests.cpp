#include "AveMediaBridge/AveMediaBridgeApi.hpp"
#include "Probe/MatroskaAacSequentialPresentation.hpp"

#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

namespace {

namespace Probe = AveMediaBridge::Probe;

bool expect(bool condition, const std::string& id, const std::string& detail = {}) {
    if (condition) {
        return true;
    }
    std::cerr << "FAIL: " << id;
    if (!detail.empty()) {
        std::cerr << " detail=\"" << detail << '"';
    }
    std::cerr << '\n';
    return false;
}

class ScopedTempDirectory {
public:
    ScopedTempDirectory() {
        std::error_code error;
        const auto ticks =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path(error) /
            (L"avemediabridge_matroska_aac_" + std::to_wstring(ticks));
        std::filesystem::create_directories(path_, error);
    }

    ~ScopedTempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

struct OpenedMedia {
    AVFormatContext* context = nullptr;
    AVStream* selected = nullptr;

    ~OpenedMedia() {
        if (context) {
            avformat_close_input(&context);
        }
    }

    OpenedMedia() = default;
    OpenedMedia(const OpenedMedia&) = delete;
    OpenedMedia& operator=(const OpenedMedia&) = delete;
};

bool openAudioOrdinal(
    const std::filesystem::path& path,
    int ordinal,
    OpenedMedia& media) {
    if (avformat_open_input(
            &media.context, path.u8string().c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(media.context, nullptr) < 0) {
        return false;
    }
    int currentOrdinal = 0;
    for (unsigned index = 0; index < media.context->nb_streams; ++index) {
        AVStream* stream = media.context->streams[index];
        if (!stream || !stream->codecpar ||
            stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        if (currentOrdinal++ == ordinal) {
            media.selected = stream;
            return true;
        }
    }
    return false;
}

std::optional<Probe::MatroskaAacSelectedStreamIdentity> identityFor(
    const std::filesystem::path& path,
    int audioOrdinal = 0) {
    OpenedMedia media;
    if (!openAudioOrdinal(path, audioOrdinal, media)) {
        return std::nullopt;
    }
    const auto eligibility = Probe::evaluateMatroskaAacSequentialEligibility(
        path.u8string(), media.context, media.selected, false);
    return eligibility.eligible
        ? std::optional<Probe::MatroskaAacSelectedStreamIdentity>{
              eligibility.selected}
        : std::nullopt;
}

std::filesystem::path findCase(
    const std::filesystem::path& root,
    const std::string& prefix) {
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(root, error), end;
         !error && it != end;
         it.increment(error)) {
        if (it->is_regular_file(error) &&
            it->path().filename().u8string().rfind(prefix, 0) == 0) {
            return it->path();
        }
    }
    return {};
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return input
        ? std::string(
              std::istreambuf_iterator<char>(input),
              std::istreambuf_iterator<char>())
        : std::string{};
}

std::optional<std::uint64_t> jsonUnsigned(
    const std::string& json,
    const std::string& key) {
    const std::string token = "\"" + key + "\"";
    std::size_t cursor = json.find(token);
    cursor = cursor == std::string::npos
        ? cursor
        : json.find(':', cursor + token.size());
    if (cursor == std::string::npos) {
        return std::nullopt;
    }
    ++cursor;
    while (cursor < json.size() &&
           std::isspace(static_cast<unsigned char>(json[cursor])) != 0) {
        ++cursor;
    }
    try {
        return static_cast<std::uint64_t>(std::stoull(json.substr(cursor)));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> jsonString(
    const std::string& json,
    const std::string& key) {
    const std::string token = "\"" + key + "\"";
    std::size_t cursor = json.find(token);
    cursor = cursor == std::string::npos
        ? cursor
        : json.find(':', cursor + token.size());
    cursor = cursor == std::string::npos
        ? cursor
        : json.find('"', cursor + 1);
    const std::size_t end = cursor == std::string::npos
        ? cursor
        : json.find('"', cursor + 1);
    return end == std::string::npos
        ? std::nullopt
        : std::optional<std::string>{
              json.substr(cursor + 1, end - cursor - 1)};
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

bool checkExact(
    const std::filesystem::path& path,
    int audioOrdinal,
    std::uint64_t expectedFrames,
    const std::string& id,
    std::uint64_t expectedAccessUnits = 0) {
    OpenedMedia media;
    bool ok = expect(openAudioOrdinal(path, audioOrdinal, media), id, "open");
    if (!media.selected) {
        return false;
    }
    const auto eligibility = Probe::evaluateMatroskaAacSequentialEligibility(
        path.u8string(), media.context, media.selected, false);
    ok &= expect(eligibility.eligible, id, "eligibility");
    if (!eligibility.eligible) {
        return false;
    }
    const auto result = Probe::probeMatroskaAacSequentialPresentation(
        path.u8string(), eligibility.selected);
    ok &= expect(
        result.exact(),
        id,
        Probe::matroskaAacSequentialReasonName(result.reason));
    ok &= expect(result.presentationFrames == expectedFrames, id, "frames");
    if (expectedAccessUnits != 0) {
        ok &= expect(
            result.selectedAccessUnits == expectedAccessUnits,
            id,
            "access units");
    }
    ok &= expect(
        result.samplesPerAccessUnit == 1024 &&
            result.aacObjectType == 2,
        id,
        "AAC-LC profile");
    ok &= expect(
        result.bytesReturned == result.fileSizeBytes &&
            result.uniqueBytes == result.fileSizeBytes &&
            result.duplicateBytes == 0 &&
            result.seekCallsAfterOpen == 0,
        id,
        "one forward-only pass");
    ok &= expect(
        result.reachedPhysicalEof &&
            result.reachedSegmentEnd &&
            result.selectedTrackMappingValid &&
            result.timestampContinuityValid &&
            result.allLacingValid &&
            result.allRelevantCrcValid &&
            result.checkedArithmeticValid,
        id,
        "proof flags");
    const auto evidence =
        Probe::makeMatroskaAacSequentialTotalPresentationEvidence(result);
    ok &= expect(
        evidence.frames == expectedFrames &&
            evidence.trust == Probe::PresentationTotalTrust::SampleExact &&
            evidence.source ==
                Probe::PresentationTotalSource::
                    MatroskaAacSequentialPresentation &&
            evidence.domain ==
                Probe::PresentationSampleDomain::NativeStreamSamples &&
            evidence.sampleRate == result.sampleRate,
        id,
        "typed authority");
    return ok;
}

bool runValidMatrix(
    const std::filesystem::path& historicalFixtures,
    const std::filesystem::path& implementationFixtures,
    const std::filesystem::path& frozenRoot,
    const std::filesystem::path& golden10) {
    struct FrozenCase {
        const char* prefix;
        std::uint64_t frames;
        std::uint64_t accessUnits;
    };
    const std::array<FrozenCase, 7> frozenCases{{
        {"EA038_", 11264, 12},
        {"EA065_", 103424, 102},
        {"EA066_", 103424, 102},
        {"EA071_", 103424, 102},
        {"EA097_", 28801024, 28127},
        {"AV014_", 142336, 140},
        {"AV022_", 144384, 142},
    }};
    bool ok = true;
    for (const auto& item : frozenCases) {
        const auto path = findCase(frozenRoot, item.prefix);
        ok &= expect(!path.empty(), item.prefix, "manifest family path");
        if (!path.empty()) {
            ok &= checkExact(
                path, 0, item.frames, item.prefix, item.accessUnits);
        }
    }

    ok &= checkExact(golden10, 0, 115200000, "golden10", 112501);
    ok &= checkExact(
        historicalFixtures / L"gen_aac_lc_44100_mono.mka",
        0,
        102400,
        "aac_lc_44100_mono",
        101);
    ok &= checkExact(
        historicalFixtures / L"gen_av_audio_ends_first.mkv",
        0,
        156672,
        "audio_ends_before_video",
        154);
    ok &= checkExact(
        historicalFixtures / L"gen_two_aac_different_lengths.mka",
        0,
        120832,
        "two_aac_short_selected",
        119);
    ok &= checkExact(
        historicalFixtures / L"gen_two_aac_different_lengths.mka",
        1,
        216064,
        "two_aac_long_selected",
        212);
    ok &= checkExact(
        historicalFixtures / L"gen_live_unknown_segment.mka",
        0,
        99328,
        "unknown_segment",
        98);
    ok &= checkExact(
        historicalFixtures / L"gen_xiph_laced_valid.mka",
        0,
        102400,
        "xiph_lacing",
        101);
    ok &= checkExact(
        implementationFixtures / L"gen_fixed_laced_valid.mka",
        0,
        102400,
        "fixed_lacing",
        101);
    ok &= checkExact(
        implementationFixtures / L"gen_ebml_laced_valid.mka",
        0,
        102400,
        "ebml_lacing",
        101);
    ok &= checkExact(
        implementationFixtures / L"gen_terminal_discard_valid.mka",
        0,
        1024,
        "terminal_discard_padding",
        3);
    ok &= checkExact(
        implementationFixtures / L"gen_codec_delay_zero_valid.mka",
        0,
        103424,
        "codec_delay_zero",
        101);
    return ok;
}

bool runRobustnessMatrix(
    const std::filesystem::path& historicalFixtures,
    const std::filesystem::path& implementationFixtures,
    const std::filesystem::path& golden10,
    std::size_t& falseExactCount) {
    const auto baseIdentity =
        identityFor(historicalFixtures / L"gen_aac_lc_44100_mono.mka");
    const auto goldenIdentity = identityFor(golden10);
    bool ok = expect(baseIdentity.has_value(), "base_identity") &&
        expect(goldenIdentity.has_value(), "golden_identity");
    if (!baseIdentity || !goldenIdentity) {
        return false;
    }

    struct FailureCase {
        const wchar_t* name;
        const Probe::MatroskaAacSelectedStreamIdentity* selected;
    };
    const std::array<FailureCase, 11> failures{{
        {L"gen_timestamp_gap.mka", &*baseIdentity},
        {L"gen_timestamp_overlap.mka", &*baseIdentity},
        {L"gen_timestamp_out_of_order.mka", &*baseIdentity},
        {L"gen_wrong_track.mka", &*baseIdentity},
        {L"gen_invalid_track_vint.mka", &*baseIdentity},
        {L"gen_invalid_lacing.mka", &*baseIdentity},
        {L"gen_invalid_crc.mka", &*baseIdentity},
        {L"gen_unknown_cluster_size.mka", &*baseIdentity},
        {L"gen_truncated_tail.mka", &*baseIdentity},
        {L"gen_missing_middle_golden.mkv", &*goldenIdentity},
        {L"gen_aac_he_unsupported.mka", &*baseIdentity},
    }};
    for (const auto& item : failures) {
        const auto result = Probe::probeMatroskaAacSequentialPresentation(
            (historicalFixtures / item.name).u8string(), *item.selected);
        falseExactCount += result.exact() ? 1U : 0U;
        ok &= expect(
            !result.exact(),
            std::filesystem::path(item.name).u8string(),
            Probe::matroskaAacSequentialReasonName(result.reason));
    }

    const auto duplicateCodecDelay =
        implementationFixtures / L"gen_duplicate_codec_delay.mka";
    const auto duplicateIdentity = identityFor(duplicateCodecDelay);
    ok &= expect(duplicateIdentity.has_value(), "duplicate_codec_delay", "identity");
    if (duplicateIdentity) {
        const auto result = Probe::probeMatroskaAacSequentialPresentation(
            duplicateCodecDelay.u8string(), *duplicateIdentity);
        falseExactCount += result.exact() ? 1U : 0U;
        ok &= expect(!result.exact(), "duplicate_codec_delay");
    }

    ok &= checkExact(
        historicalFixtures / L"gen_missing_final_valid.mka",
        0,
        101376,
        "valid_shorter_selected_track",
        100);

    auto wrongTrack = *baseIdentity;
    wrongTrack.trackNumber = 99;
    const auto wrongTrackResult =
        Probe::probeMatroskaAacSequentialPresentation(
            (historicalFixtures / L"gen_aac_lc_44100_mono.mka").u8string(),
            wrongTrack);
    falseExactCount += wrongTrackResult.exact() ? 1U : 0U;
    ok &= expect(!wrongTrackResult.exact(), "wrong_selected_track");

    auto wrongOrdinal = *baseIdentity;
    wrongOrdinal.audioOrdinal = 99;
    const auto wrongOrdinalResult =
        Probe::probeMatroskaAacSequentialPresentation(
            (historicalFixtures / L"gen_two_aac_different_lengths.mka").u8string(),
            wrongOrdinal);
    falseExactCount += wrongOrdinalResult.exact() ? 1U : 0U;
    ok &= expect(!wrongOrdinalResult.exact(), "wrong_selected_audio_ordinal");

    Probe::MatroskaAacSequentialTestHooks hooks;
    hooks.forceReadErrorAfterBytes = 4096;
    Probe::MatroskaAacSequentialProbeOptions options;
    options.testHooks = &hooks;
    const auto forcedRead = Probe::probeMatroskaAacSequentialPresentation(
        (historicalFixtures / L"gen_aac_lc_44100_mono.mka").u8string(),
        *baseIdentity,
        options);
    falseExactCount += forcedRead.exact() ? 1U : 0U;
    ok &= expect(
        forcedRead.status == Probe::MatroskaAacSequentialStatus::IoError &&
            forcedRead.reason ==
                Probe::MatroskaAacSequentialReason::ForcedReadError,
        "forced_read_error");

    OpenedMedia base;
    const auto basePath = historicalFixtures / L"gen_aac_lc_44100_mono.mka";
    ok &= expect(openAudioOrdinal(basePath, 0, base), "stronger_exact_open");
    if (base.selected) {
        const auto ineligible =
            Probe::evaluateMatroskaAacSequentialEligibility(
                basePath.u8string(), base.context, base.selected, true);
        ok &= expect(!ineligible.eligible, "stronger_exact_preserved");
    }

    return ok;
}

bool runFallbackEligibility(
    const std::filesystem::path& historicalFixtures,
    const std::filesystem::path& frozenRoot) {
    bool ok = true;
    OpenedMedia heAac;
    const auto hePath = historicalFixtures / L"gen_aac_he_unsupported.mka";
    if (openAudioOrdinal(hePath, 0, heAac)) {
        const auto eligibility =
            Probe::evaluateMatroskaAacSequentialEligibility(
                hePath.u8string(), heAac.context, heAac.selected, false);
        ok &= expect(!eligibility.eligible, "he_aac_early_fallback");
    }

    const auto opusPath =
        findCase(frozenRoot, "EA020_");
    if (!opusPath.empty()) {
        OpenedMedia nonAac;
        if (openAudioOrdinal(opusPath, 0, nonAac)) {
            const auto eligibility =
                Probe::evaluateMatroskaAacSequentialEligibility(
                    opusPath.u8string(),
                    nonAac.context,
                    nonAac.selected,
                    false);
            ok &= expect(!eligibility.eligible, "non_aac_excluded");
        }
    }
    return ok;
}

bool runProductIntegration(
    const std::filesystem::path& golden10) {
    ScopedTempDirectory temp;
    const auto output = temp.path() / L"probe.json";
    bool ok = expect(
        AveMediaBridge_ProbeToJson(golden10.c_str(), output.c_str()) == 0,
        "product_probe");
    const std::string json = readText(output);
    ok &= expect(
        jsonUnsigned(json, "decodedSampleFrames") == 115200000,
        "product_loading_frames");
    ok &= expect(
        jsonString(json, "decodedSampleFramesSource") ==
            "matroska_aac_sequential_presentation",
        "product_typed_source");
    ok &= expect(
        jsonString(json, "matroskaAacSequentialStatus") == "exact",
        "product_sequential_exact");
    ok &= expect(
        jsonBool(json, "matroskaAacSequentialGenericScanEntered") == false &&
            jsonBool(json, "matroskaAacSequentialGenericScanSkipped") == true &&
            jsonBool(json, "matroskaAacSequentialPossibleDoublePass") == false,
        "product_old_scan_skipped");
    ok &= expect(
        jsonUnsigned(json, "matroskaAacSequentialSelectedAccessUnits") == 112501 &&
            jsonUnsigned(json, "matroskaAacSequentialPhysicalFrames") == 115201024 &&
            jsonUnsigned(json, "matroskaAacSequentialInitialSkipFrames") == 1024,
        "product_authority_components");
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr
            << "usage: test <historical-fixtures> <implementation-fixtures> "
               "<frozen-root> <golden10>\n";
        return 2;
    }

    const std::filesystem::path historicalFixtures =
        std::filesystem::u8path(argv[1]);
    const std::filesystem::path implementationFixtures =
        std::filesystem::u8path(argv[2]);
    const std::filesystem::path frozenRoot =
        std::filesystem::u8path(argv[3]);
    const std::filesystem::path golden10 =
        std::filesystem::u8path(argv[4]);
    if (!std::filesystem::is_directory(historicalFixtures) ||
        !std::filesystem::is_directory(implementationFixtures) ||
        !std::filesystem::is_directory(frozenRoot) ||
        !std::filesystem::is_regular_file(golden10)) {
        std::cout << "SKIP: external read-only media fixtures unavailable\n";
        return 77;
    }

    bool ok = true;
    std::size_t falseExactCount = 0;
    ok &= runValidMatrix(
        historicalFixtures,
        implementationFixtures,
        frozenRoot,
        golden10);
    ok &= runRobustnessMatrix(
        historicalFixtures,
        implementationFixtures,
        golden10,
        falseExactCount);
    ok &= runFallbackEligibility(historicalFixtures, frozenRoot);
    ok &= runProductIntegration(golden10);
    ok &= expect(falseExactCount == 0, "false_exact_count");
    ok &= expect(
        std::string(Probe::presentationTotalSourceName(
            Probe::PresentationTotalSource::
                MatroskaAacSequentialPresentation)) ==
            "matroska_aac_sequential_presentation",
        "source_name");

    if (!ok) {
        return 1;
    }
    std::cout
        << "AVEMEDIABRIDGE_MATROSKA_AAC_SEQUENTIAL_PRESENTATION_AUTHORITY_OK\n";
    return 0;
}
