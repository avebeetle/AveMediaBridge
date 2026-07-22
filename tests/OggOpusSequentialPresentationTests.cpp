#include "AveMediaBridge/AveMediaBridgeApi.hpp"
#include "Probe/OggOpusSequentialPresentation.hpp"

#include <algorithm>
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
#include <utility>
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
    explicit ScopedTempDirectory(const wchar_t* prefix) {
        std::error_code error;
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path(error) /
            (std::wstring(prefix) + L"_" + std::to_wstring(ticks));
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

    OpenedMedia(const OpenedMedia&) = delete;
    OpenedMedia& operator=(const OpenedMedia&) = delete;
    OpenedMedia() = default;
};

bool openSelected(const std::filesystem::path& path, OpenedMedia& media) {
    if (avformat_open_input(&media.context, path.u8string().c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(media.context, nullptr) < 0) {
        return false;
    }
    const int index = av_find_best_stream(
        media.context, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (index < 0) {
        return false;
    }
    media.selected = media.context->streams[index];
    return media.selected != nullptr;
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return input
        ? std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>())
        : std::string{};
}

bool writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return output.good();
}

std::vector<char> readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return input
        ? std::vector<char>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>())
        : std::vector<char>{};
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
        : std::optional<std::string>{json.substr(cursor + 1, end - cursor - 1)};
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

bool scanExpected(
    const std::filesystem::path& path,
    std::uint64_t expectedFrames,
    const std::string& id,
    std::size_t readBufferBytes = Probe::kOggOpusSequentialReadBufferBytes) {
    OpenedMedia media;
    if (!openSelected(path, media)) {
        return expect(false, id, "FFmpeg could not select audio stream");
    }
    const Probe::OggOpusSequentialEligibility eligibility =
        Probe::evaluateOggOpusSequentialEligibility(
            path.u8string(), media.context, media.selected, false);
    bool ok = expect(eligibility.eligible, id, "eligibility rejected");
    if (!eligibility.eligible) {
        return false;
    }
    Probe::OggOpusSequentialProbeOptions options;
    options.readBufferBytes = readBufferBytes;
    const Probe::OggOpusSequentialPresentationResult result =
        Probe::probeOggOpusSequentialPresentation(
            path.u8string(), eligibility.selected, options);
    ok &= expect(result.exact(), id, Probe::oggOpusSequentialReasonName(result.reason));
    ok &= expect(result.presentationFrames == expectedFrames, id, "presentation frames");
    ok &= expect(result.bytesReturned == result.fileSizeBytes, id, "physical coverage");
    ok &= expect(result.uniqueBytes == result.fileSizeBytes && result.duplicateBytes == 0,
        id, "unique coverage");
    ok &= expect(result.reachedPhysicalEof && result.allPageCrcValid,
        id, "EOF/CRC proof");
    ok &= expect(result.selectedSequenceContinuous && result.packetContinuityValid,
        id, "continuity proof");
    ok &= expect(result.finalGranuleInPacketInterval, id, "final granule interval");
    const Probe::TotalPresentationEvidence evidence =
        Probe::makeOggOpusSequentialTotalPresentationEvidence(result);
    ok &= expect(
        evidence.frames == expectedFrames &&
            evidence.trust == Probe::PresentationTotalTrust::SampleExact &&
            evidence.source == Probe::PresentationTotalSource::OggOpusSequentialPresentation &&
            evidence.domain == Probe::PresentationSampleDomain::NativeStreamSamples &&
            evidence.sampleRate == 48000,
        id, "typed authority");
    return ok;
}

bool runPacketDurationMatrix() {
    struct Case {
        const char* id;
        std::vector<std::uint8_t> packet;
        int streams;
        int samples;
    };
    const std::array<Case, 10> cases{
        Case{"2_5ms", {static_cast<std::uint8_t>(16 << 3), 0}, 1, 120},
        Case{"5ms", {static_cast<std::uint8_t>(17 << 3), 0}, 1, 240},
        Case{"10ms", {static_cast<std::uint8_t>(18 << 3), 0}, 1, 480},
        Case{"20ms", {static_cast<std::uint8_t>(19 << 3), 0}, 1, 960},
        Case{"40ms", {static_cast<std::uint8_t>(2 << 3), 0}, 1, 1920},
        Case{"60ms", {static_cast<std::uint8_t>(3 << 3), 0}, 1, 2880},
        Case{"two_cbr", {static_cast<std::uint8_t>((19 << 3) | 1), 0, 0}, 1, 1920},
        Case{"many_cbr", {static_cast<std::uint8_t>((16 << 3) | 3), 4, 0, 0, 0, 0}, 1, 480},
        Case{"two_vbr", {static_cast<std::uint8_t>((19 << 3) | 3), 0x82, 1, 0, 0}, 1, 1920},
        Case{"two_stream", {static_cast<std::uint8_t>(19 << 3), 1, 0,
                            static_cast<std::uint8_t>(19 << 3), 0}, 2, 960},
    };
    bool ok = true;
    for (const Case& item : cases) {
        const Probe::OggOpusPacketDurationResult result =
            Probe::parseOggOpusPacketDuration(
                item.packet.data(), item.packet.size(), item.streams);
        ok &= expect(result.valid && result.samples48k == item.samples,
            item.id, "packet duration");
    }
    const std::array<std::vector<std::uint8_t>, 4> invalid{
        std::vector<std::uint8_t>{},
        std::vector<std::uint8_t>{static_cast<std::uint8_t>((19 << 3) | 3), 0},
        std::vector<std::uint8_t>{static_cast<std::uint8_t>((19 << 3) | 2), 1},
        std::vector<std::uint8_t>{static_cast<std::uint8_t>((19 << 3) | 2), 252},
    };
    for (std::size_t index = 0; index < invalid.size(); ++index) {
        ok &= expect(
            !Probe::parseOggOpusPacketDuration(
                invalid[index].data(), invalid[index].size(), 1).valid,
            "invalid_packet_" + std::to_string(index));
    }
    return ok;
}

bool runValidMatrix(
    const std::filesystem::path& fixtureRoot,
    const std::filesystem::path& frozenRoot,
    const std::filesystem::path& golden07) {
    struct Case {
        std::filesystem::path path;
        std::uint64_t frames;
        const char* id;
    };
    const std::array<Case, 24> cases{
        Case{golden07, 115200000, "GOLDEN07"},
        Case{frozenRoot / L"exact_authority/valid/opus/EA044_ogg_opus_24000_24000.opus", 48000, "EA044"},
        Case{frozenRoot / L"exact_authority/valid/opus/EA062_ogg_opus_48000_102537.opus", 102537, "EA062"},
        Case{frozenRoot / L"exact_authority/valid/opus/EA095_ogg_opus_48000_28801023.opus", 28801023, "EA095"},
        Case{frozenRoot / L"exact_authority/valid/opus/EA101_ogg_opus_48000_31.opus", 31, "EA101"},
        Case{frozenRoot / L"exact_authority/valid/opus/EA148_ogg_opus_12000_100003.opus", 400012, "EA148"},
        Case{fixtureRoot / L"gen_one_packet.opus", 31, "one_packet"},
        Case{fixtureRoot / L"gen_boundary_20ms.opus", 960, "boundary"},
        Case{fixtureRoot / L"gen_nonaligned_100003.opus", 100003, "nonaligned"},
        Case{fixtureRoot / L"gen_medium_2500ms.opus", 120000, "medium"},
        Case{fixtureRoot / L"gen_long_120s.opus", 5760000, "long"},
        Case{fixtureRoot / L"gen_audio_short_1s.opus", 48000, "audio_short"},
        Case{fixtureRoot / L"gen_audio_long_3s.opus", 144000, "audio_long"},
        Case{fixtureRoot / L"gen_two_audio_selected_short.ogg", 48000, "two_audio_short"},
        Case{fixtureRoot / L"gen_two_audio_selected_long.ogg", 144000, "two_audio_long"},
        Case{fixtureRoot / L"gen_av_audio_first.ogv", 48000, "av_audio_first"},
        Case{fixtureRoot / L"gen_av_audio_last.ogv", 144000, "av_audio_last"},
        Case{fixtureRoot / L"gen_continued_opus_tags.opus", 31, "continued_tags"},
        Case{fixtureRoot / L"gen_frame_2_5ms.opus", 59259, "frame_2_5ms"},
        Case{fixtureRoot / L"gen_frame_5ms.opus", 59259, "frame_5ms"},
        Case{fixtureRoot / L"gen_frame_10ms.opus", 59259, "frame_10ms"},
        Case{fixtureRoot / L"gen_frame_40ms.opus", 59259, "frame_40ms"},
        Case{fixtureRoot / L"gen_frame_60ms.opus", 59259, "frame_60ms"},
        Case{fixtureRoot / L"gen_vbr_20ms.opus", 59259, "vbr_20ms"},
    };
    bool ok = true;
    for (const Case& item : cases) {
        ok &= scanExpected(item.path, item.frames, item.id);
    }
    for (const std::size_t buffer : std::array<std::size_t, 6>{1, 17, 27, 31, 255, 4095}) {
        ok &= scanExpected(
            fixtureRoot / L"gen_continued_opus_tags.opus",
            31,
            "split_buffer_" + std::to_string(buffer),
            buffer);
    }
    return ok;
}

bool runRobustnessMatrix(
    const std::filesystem::path& fixtureRoot,
    const std::filesystem::path& frozenRoot,
    std::size_t& falseExactCount) {
    OpenedMedia base;
    const std::filesystem::path valid = fixtureRoot / L"gen_medium_2500ms.opus";
    if (!openSelected(valid, base)) {
        return expect(false, "robustness_base_open");
    }
    const Probe::OggOpusSequentialEligibility eligibility =
        Probe::evaluateOggOpusSequentialEligibility(
            valid.u8string(), base.context, base.selected, false);
    if (!eligibility.eligible) {
        return expect(false, "robustness_base_eligibility");
    }

    const std::array<const wchar_t*, 20> files{
        L"adv_missing_eos.opus", L"adv_unknown_granule.opus",
        L"adv_granule_before_preskip.opus", L"adv_wrong_serial_eos.opus",
        L"adv_duplicate_selected_bos.opus", L"adv_tail_sequence_gap.opus",
        L"adv_stale_eos_too_late.opus", L"adv_stale_eos_too_early.opus",
        L"adv_invalid_crc.opus", L"adv_truncated_header.opus",
        L"adv_truncated_segment_table.opus", L"adv_truncated_body.opus",
        L"adv_trailing_garbage.opus", L"adv_selected_page_after_eos.opus",
        L"adv_unsupported_opus_head_version.opus",
        L"adv_missing_opus_head.opus", L"adv_missing_middle_page.opus",
        L"adv_invalid_opus_packet.opus", L"adv_zero_byte.bin", L"adv_non_ogg.bin"};
    bool ok = true;
    for (const wchar_t* name : files) {
        const Probe::OggOpusSequentialPresentationResult result =
            Probe::probeOggOpusSequentialPresentation(
                (fixtureRoot / name).u8string(), eligibility.selected);
        falseExactCount += result.exact() ? 1U : 0U;
        ok &= expect(!result.exact(), std::filesystem::path(name).u8string(),
            Probe::oggOpusSequentialReasonName(result.reason));
    }

    const auto fakeCapturePattern = Probe::probeOggOpusSequentialPresentation(
        (fixtureRoot / L"adv_fake_oggs_payload.opus").u8string(), eligibility.selected);
    ok &= expect(
        fakeCapturePattern.exact() && fakeCapturePattern.presentationFrames == 120000,
        "fake_oggs_inside_packet_payload",
        Probe::oggOpusSequentialReasonName(fakeCapturePattern.reason));

    const Probe::OggOpusSequentialPresentationResult chained =
        Probe::probeOggOpusSequentialPresentation(
            (fixtureRoot / L"gen_chained.opus").u8string(), eligibility.selected);
    falseExactCount += chained.exact() ? 1U : 0U;
    ok &= expect(
        chained.status == Probe::OggOpusSequentialStatus::Chained && !chained.exact(),
        "chained_fallback", Probe::oggOpusSequentialReasonName(chained.reason));

    Probe::OggOpusSelectedStreamIdentity wrong = eligibility.selected;
    wrong.opusStreamOrdinal = 99;
    const Probe::OggOpusSequentialPresentationResult wrongSelected =
        Probe::probeOggOpusSequentialPresentation(
            (fixtureRoot / L"gen_two_audio_selected_short.ogg").u8string(), wrong);
    falseExactCount += wrongSelected.exact() ? 1U : 0U;
    ok &= expect(!wrongSelected.exact(), "wrong_selected_stream");

    Probe::OggOpusSequentialTestHooks readErrorHooks;
    readErrorHooks.forceReadErrorAfterBytes = 4096;
    Probe::OggOpusSequentialProbeOptions readErrorOptions;
    readErrorOptions.testHooks = &readErrorHooks;
    const auto readError = Probe::probeOggOpusSequentialPresentation(
        valid.u8string(), eligibility.selected, readErrorOptions);
    falseExactCount += readError.exact() ? 1U : 0U;
    ok &= expect(
        readError.status == Probe::OggOpusSequentialStatus::IoError && !readError.exact(),
        "forced_read_error");

    Probe::OggOpusSequentialTestHooks packetLimitHooks;
    packetLimitHooks.selectedPacketMaximumOverride = 16;
    Probe::OggOpusSequentialProbeOptions packetLimitOptions;
    packetLimitOptions.testHooks = &packetLimitHooks;
    const auto packetLimit = Probe::probeOggOpusSequentialPresentation(
        valid.u8string(), eligibility.selected, packetLimitOptions);
    falseExactCount += packetLimit.exact() ? 1U : 0U;
    ok &= expect(!packetLimit.exact(), "packet_limit");

    OpenedMedia vorbis;
    const std::filesystem::path vorbisPath =
        frozenRoot / L"exact_authority/valid/vorbis/EA102_ogg_vorbis_48000_63.ogg";
    ok &= expect(openSelected(vorbisPath, vorbis), "vorbis_open");
    if (vorbis.selected) {
        const auto vorbisEligibility = Probe::evaluateOggOpusSequentialEligibility(
            vorbisPath.u8string(), vorbis.context, vorbis.selected, false);
        ok &= expect(!vorbisEligibility.eligible, "vorbis_excluded");
    }
    return ok;
}

bool runProbeAndHandoffIntegration(
    const std::filesystem::path& media,
    std::uint64_t expectedFrames) {
    ScopedTempDirectory temp(L"avemediabridge_ogg_opus_sequential");
    const std::filesystem::path exactSession = temp.path() / L"exact";
    const std::filesystem::path fallbackSession = temp.path() / L"fallback";
    const std::filesystem::path staleSession = temp.path() / L"stale";
    std::error_code error;
    std::filesystem::create_directories(exactSession, error);
    std::filesystem::create_directories(fallbackSession, error);
    std::filesystem::create_directories(staleSession, error);
    bool ok = expect(!error, "integration_temp_directory", error.message());

    const std::filesystem::path probePath = exactSession / L"probe.json";
    ok &= expect(AveMediaBridge_ProbeToJson(media.c_str(), probePath.c_str()) == 0,
        "integration_probe");
    const std::string probe = readText(probePath);
    ok &= expect(jsonUnsigned(probe, "decodedSampleFrames") == expectedFrames,
        "integration_loading");
    ok &= expect(jsonString(probe, "decodedSampleFramesSource") ==
        "ogg_opus_sequential_presentation", "integration_source");
    ok &= expect(jsonBool(probe, "oggOpusSequentialGenericScanEntered") == false,
        "integration_old_scan_skipped");

    AveMediaBridgeImportOptions exactOptions{};
    exactOptions.structSize = sizeof(exactOptions);
    exactOptions.inputPath = media.c_str();
    exactOptions.sessionMediaDir = exactSession.c_str();
    ok &= expect(AveMediaBridge_ImportAudioToSessionEx(&exactOptions) == 0,
        "integration_exact_import");

    AveMediaBridgeImportOptions fallbackOptions{};
    fallbackOptions.structSize = sizeof(fallbackOptions);
    fallbackOptions.inputPath = media.c_str();
    fallbackOptions.sessionMediaDir = fallbackSession.c_str();
    ok &= expect(AveMediaBridge_ImportAudioToSessionEx(&fallbackOptions) == 0,
        "integration_fallback_import");

    std::string staleProbe = probe;
    const std::string fileSizeToken = "\"oggOpusSequentialFileSizeBytes\": ";
    const std::size_t fileSizePosition = staleProbe.find(fileSizeToken);
    ok &= expect(fileSizePosition != std::string::npos,
        "integration_stale_identity_field");
    if (fileSizePosition != std::string::npos) {
        const std::size_t valueBegin = fileSizePosition + fileSizeToken.size();
        const std::size_t valueEnd = staleProbe.find(',', valueBegin);
        const auto fileSize = jsonUnsigned(staleProbe, "oggOpusSequentialFileSizeBytes");
        ok &= expect(valueEnd != std::string::npos && fileSize.has_value(),
            "integration_stale_identity_parse");
        if (valueEnd != std::string::npos && fileSize.has_value()) {
            staleProbe.replace(
                valueBegin,
                valueEnd - valueBegin,
                std::to_string(*fileSize + 1));
        }
    }
    ok &= expect(writeText(staleSession / L"probe.json", staleProbe),
        "integration_stale_probe_write");
    AveMediaBridgeImportOptions staleOptions{};
    staleOptions.structSize = sizeof(staleOptions);
    staleOptions.inputPath = media.c_str();
    staleOptions.sessionMediaDir = staleSession.c_str();
    ok &= expect(AveMediaBridge_ImportAudioToSessionEx(&staleOptions) == 0,
        "integration_stale_import");

    const std::string exactInfo = readText(exactSession / L"audio_info.json");
    const std::string fallbackInfo = readText(fallbackSession / L"audio_info.json");
    const std::string exactMetadata = readText(exactSession / L"metadata.json");
    const std::string fallbackMetadata = readText(fallbackSession / L"metadata.json");
    const std::string staleInfo = readText(staleSession / L"audio_info.json");
    const std::string staleMetadata = readText(staleSession / L"metadata.json");
    ok &= expect(jsonUnsigned(exactInfo, "frames") == expectedFrames,
        "integration_exact_ready");
    ok &= expect(jsonUnsigned(fallbackInfo, "frames") == expectedFrames,
        "integration_fallback_ready");
    ok &= expect(jsonString(exactMetadata, "source") ==
        "ogg_opus_sequential_presentation", "integration_budget_source");
    ok &= expect(jsonBool(exactMetadata, "packetScanExecuted") == false,
        "integration_duplicate_scan");
    ok &= expect(jsonBool(exactMetadata, "oggOpusSequentialAuthorityReused") == true,
        "integration_handoff_reused");
    ok &= expect(jsonBool(exactMetadata, "duplicatePresentationScanEntered") == false,
        "integration_duplicate_diagnostic");
    ok &= expect(jsonBool(fallbackMetadata, "packetScanExecuted") == true,
        "integration_fallback_scan");
    ok &= expect(jsonUnsigned(staleInfo, "frames") == expectedFrames,
        "integration_stale_ready");
    ok &= expect(jsonBool(staleMetadata, "packetScanExecuted") == true,
        "integration_stale_fallback_scan");
    ok &= expect(jsonBool(staleMetadata, "oggOpusSequentialAuthorityReused") == false,
        "integration_stale_handoff_rejected");
    ok &= expect(
        readBytes(exactSession / L"original_f32.bin") ==
            readBytes(fallbackSession / L"original_f32.bin"),
        "integration_pcm_parity");
    ok &= expect(
        readBytes(exactSession / L"original_f32.bin") ==
            readBytes(staleSession / L"original_f32.bin"),
        "integration_stale_pcm_parity");
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cout << "AVEMEDIABRIDGE_OGG_OPUS_SEQUENTIAL_PRESENTATION_SKIPPED: "
                     "fixture root, frozen root, and Golden 07 are required\n";
        return 77;
    }
    const std::filesystem::path fixtureRoot = std::filesystem::u8path(argv[1]);
    const std::filesystem::path frozenRoot = std::filesystem::u8path(argv[2]);
    const std::filesystem::path golden07 = std::filesystem::u8path(argv[3]);
    if (!std::filesystem::is_directory(fixtureRoot) ||
        !std::filesystem::is_directory(frozenRoot) ||
        !std::filesystem::is_regular_file(golden07)) {
        std::cout << "AVEMEDIABRIDGE_OGG_OPUS_SEQUENTIAL_PRESENTATION_SKIPPED: "
                     "required media unavailable\n";
        return 77;
    }

    bool ok = runPacketDurationMatrix();
    ok &= runValidMatrix(fixtureRoot, frozenRoot, golden07);
    std::size_t falseExactCount = 0;
    ok &= runRobustnessMatrix(fixtureRoot, frozenRoot, falseExactCount);
    ok &= runProbeAndHandoffIntegration(
        frozenRoot / L"exact_authority/valid/opus/EA062_ogg_opus_48000_102537.opus",
        102537);
    ok &= expect(falseExactCount == 0, "false_exact_count");
    if (!ok) {
        return 1;
    }
    std::cout << "AVEMEDIABRIDGE_OGG_OPUS_SEQUENTIAL_PRESENTATION_AUTHORITY_OK "
              << "validExactCases=24 falseExactCount=" << falseExactCount
              << " genericHealthyScanEntered=no duplicatePresentationScanEntered=no\n";
    return 0;
}
