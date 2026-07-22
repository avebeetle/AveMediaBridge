#include "AveMediaBridge/AveMediaBridgeApi.hpp"
#include "Probe/AdtsAacSequentialPresentation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
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

std::vector<char> readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return input
        ? std::vector<char>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>())
        : std::vector<char>{};
}

bool writeBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

bool writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return output.good();
}

std::optional<std::uint64_t> jsonUnsigned(
    const std::string& json,
    const std::string& key) {
    const std::string token = "\"" + key + "\"";
    std::size_t cursor = json.find(token);
    cursor = cursor == std::string::npos
        ? cursor : json.find(':', cursor + token.size());
    cursor = cursor == std::string::npos
        ? cursor : json.find_first_not_of(" \t\r\n", cursor + 1);
    if (cursor == std::string::npos) {
        return std::nullopt;
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
        ? cursor : json.find(':', cursor + token.size());
    cursor = cursor == std::string::npos
        ? cursor : json.find('"', cursor + 1);
    const std::size_t end = cursor == std::string::npos
        ? cursor : json.find('"', cursor + 1);
    return end == std::string::npos
        ? std::nullopt
        : std::optional<std::string>{json.substr(cursor + 1, end - cursor - 1)};
}

std::optional<bool> jsonBool(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    std::size_t cursor = json.find(token);
    cursor = cursor == std::string::npos
        ? cursor : json.find(':', cursor + token.size());
    cursor = cursor == std::string::npos
        ? cursor : json.find_first_not_of(" \t\r\n", cursor + 1);
    if (cursor != std::string::npos && json.compare(cursor, 4, "true") == 0) {
        return true;
    }
    if (cursor != std::string::npos && json.compare(cursor, 5, "false") == 0) {
        return false;
    }
    return std::nullopt;
}

struct FrameConfig {
    int mpegId = 0;
    int profileBits = 1;
    int sampleRateIndex = 4;
    int channels = 2;
    bool protectionAbsent = true;
    int rawDataBlockField = 0;
    std::size_t payloadBytes = 23;
    std::uint8_t payloadFill = 0x55;
};

std::vector<std::uint8_t> makeFrame(const FrameConfig& config) {
    const std::size_t headerBytes = config.protectionAbsent ? 7 : 9;
    const std::size_t frameBytes = headerBytes + config.payloadBytes;
    std::vector<std::uint8_t> result(frameBytes, config.payloadFill);
    result[0] = 0xff;
    result[1] = static_cast<std::uint8_t>(
        0xf0U | ((config.mpegId & 1) << 3U) |
        (config.protectionAbsent ? 1U : 0U));
    result[2] = static_cast<std::uint8_t>(
        ((config.profileBits & 3) << 6U) |
        ((config.sampleRateIndex & 15) << 2U) |
        ((config.channels >> 2U) & 1U));
    result[3] = static_cast<std::uint8_t>(
        ((config.channels & 3) << 6U) | ((frameBytes >> 11U) & 3U));
    result[4] = static_cast<std::uint8_t>((frameBytes >> 3U) & 0xffU);
    result[5] = static_cast<std::uint8_t>(((frameBytes & 7U) << 5U) | 0x1fU);
    result[6] = static_cast<std::uint8_t>(0xfcU | (config.rawDataBlockField & 3));
    return result;
}

void append(std::vector<std::uint8_t>& destination, const std::vector<std::uint8_t>& source) {
    destination.insert(destination.end(), source.begin(), source.end());
}

std::vector<std::uint8_t> makeStream(
    const FrameConfig& config,
    std::size_t frames) {
    std::vector<std::uint8_t> result;
    for (std::size_t index = 0; index < frames; ++index) {
        FrameConfig current = config;
        current.payloadBytes += index % 5;
        current.payloadFill = static_cast<std::uint8_t>(0x40U + index % 31U);
        append(result, makeFrame(current));
    }
    return result;
}

Probe::AdtsAacSelectedStreamIdentity selectedIdentity(
    int sampleRate = 44100,
    int channels = 2) {
    Probe::AdtsAacSelectedStreamIdentity result;
    result.streamIndex = 0;
    result.sampleRate = sampleRate;
    result.channels = channels;
    result.codecProfile = AV_PROFILE_AAC_LOW;
    result.codecFrameSize = 1024;
    return result;
}

Probe::AdtsAacSequentialPresentationResult scan(
    const std::filesystem::path& path,
    int sampleRate,
    int channels,
    std::size_t bufferBytes = Probe::kAdtsAacSequentialReadBufferBytes,
    const Probe::AdtsAacSequentialTestHooks* hooks = nullptr) {
    Probe::AdtsAacSequentialProbeOptions options;
    options.readBufferBytes = bufferBytes;
    options.testHooks = hooks;
    return Probe::probeAdtsAacSequentialPresentation(
        path.u8string(), selectedIdentity(sampleRate, channels), options);
}

bool expectExact(
    const Probe::AdtsAacSequentialPresentationResult& result,
    std::uint64_t frames,
    const std::string& id) {
    const Probe::TotalPresentationEvidence evidence =
        Probe::makeAdtsAacSequentialTotalPresentationEvidence(result);
    return expect(result.exact(), id, Probe::adtsAacSequentialReasonName(result.reason)) &&
        expect(result.presentationFrames == frames && result.physicalFrames == frames,
            id + "_frames") &&
        expect(result.bytesReturned == result.fileSizeBytes &&
               result.uniqueBytes == result.fileSizeBytes && result.duplicateBytes == 0,
            id + "_io") &&
        expect(result.seekCallsAfterOpen == 0 && result.reachedPhysicalEof,
            id + "_forward_eof") &&
        expect(result.frameBoundariesValid && result.configurationContinuous &&
               result.outputDomainValidated && result.checkedArithmeticValid &&
               result.fileIdentityStable,
            id + "_proof") &&
        expect(evidence.frames == frames &&
               evidence.trust == Probe::PresentationTotalTrust::SampleExact &&
               evidence.source ==
                   Probe::PresentationTotalSource::AdtsAacSequentialPresentation &&
               evidence.domain == Probe::PresentationSampleDomain::NativeStreamSamples &&
               evidence.validation ==
                   Probe::PresentationTotalValidation::SelfContainedMetadata,
            id + "_typed_evidence");
}

bool runValidMatrix(const std::filesystem::path& root) {
    struct Case {
        const char* id;
        int sampleRateIndex;
        int sampleRate;
        int channels;
        int mpegId;
        std::size_t frames;
        std::size_t payload;
        std::size_t buffer;
    };
    const std::array<Case, 13> cases{{
        {"mpeg4_44100_stereo", 4, 44100, 2, 0, 1, 23, 262144},
        {"mpeg2_44100_stereo", 4, 44100, 2, 1, 2, 31, 262144},
        {"48000_mono", 3, 48000, 1, 0, 3, 41, 262144},
        {"32000_stereo", 5, 32000, 2, 0, 5, 53, 262144},
        {"24000_mono", 6, 24000, 1, 0, 7, 67, 262144},
        {"12000_stereo", 9, 12000, 2, 0, 9, 79, 262144},
        {"22050_mono", 7, 22050, 1, 0, 11, 83, 262144},
        {"96000_stereo", 0, 96000, 2, 0, 13, 97, 262144},
        {"16000_six_channel", 8, 16000, 6, 0, 4, 113, 262144},
        {"short_boundary", 4, 44100, 2, 0, 17, 101, 19},
        {"header_split", 4, 44100, 2, 0, 19, 6, 10},
        {"payload_split", 4, 44100, 2, 0, 23, 1000, 37},
        {"medium_variable", 4, 44100, 2, 0, 1001, 127, 4096},
    }};
    bool ok = true;
    for (const Case& item : cases) {
        FrameConfig config;
        config.mpegId = item.mpegId;
        config.sampleRateIndex = item.sampleRateIndex;
        config.channels = item.channels;
        config.payloadBytes = item.payload;
        const std::filesystem::path path = root / (std::string(item.id) + ".aac");
        ok &= expect(writeBytes(path, makeStream(config, item.frames)), item.id, "write");
        ok &= expectExact(
            scan(path, item.sampleRate, item.channels, item.buffer),
            item.frames * 1024ULL,
            item.id);
    }
    return ok;
}

bool runInventorySemantics(const std::filesystem::path& root) {
    FrameConfig config;
    std::vector<std::uint8_t> base = makeStream(config, 4);
    const std::vector<std::uint8_t> frame = makeFrame(config);
    bool ok = true;

    std::vector<std::uint8_t> missing;
    append(missing, makeFrame(config));
    append(missing, makeFrame(config));
    append(missing, makeFrame(config));
    const auto missingPath = root / L"missing_complete.aac";
    ok &= writeBytes(missingPath, missing);
    ok &= expectExact(scan(missingPath, 44100, 2), 3 * 1024ULL, "missing_frame_inventory");

    std::vector<std::uint8_t> duplicate = base;
    append(duplicate, frame);
    const auto duplicatePath = root / L"duplicate_complete.aac";
    ok &= writeBytes(duplicatePath, duplicate);
    ok &= expectExact(scan(duplicatePath, 44100, 2), 5 * 1024ULL, "duplicate_frame_inventory");

    std::vector<std::uint8_t> payloadSync = makeStream(config, 3);
    payloadSync[10] = 0xff;
    payloadSync[11] = 0xf1;
    const auto syncPath = root / L"payload_sync.aac";
    ok &= writeBytes(syncPath, payloadSync);
    ok &= expectExact(scan(syncPath, 44100, 2), 3 * 1024ULL, "payload_sync_ignored");

    std::vector<std::uint8_t> payloadCorrupt = makeStream(config, 2);
    payloadCorrupt[15] ^= 0x7f;
    const auto payloadPath = root / L"payload_corrupt.aac";
    ok &= writeBytes(payloadPath, payloadCorrupt);
    ok &= expectExact(scan(payloadPath, 44100, 2), 2 * 1024ULL,
        "payload_corruption_preserves_declared_extent");
    return ok;
}

bool expectNonExact(
    const Probe::AdtsAacSequentialPresentationResult& result,
    const std::string& id,
    std::size_t& falseExactCount) {
    if (result.exact()) {
        ++falseExactCount;
    }
    return expect(!result.exact(), id, Probe::adtsAacSequentialReasonName(result.reason));
}

bool runRobustnessMatrix(
    const std::filesystem::path& root,
    std::size_t& falseExactCount) {
    bool ok = true;
    FrameConfig base;
    auto run = [&](const std::string& id, const std::vector<std::uint8_t>& bytes,
                   int rate = 44100, int channels = 2) {
        const auto path = root / (id + ".aac");
        ok &= expect(writeBytes(path, bytes), id + "_write");
        ok &= expectNonExact(scan(path, rate, channels), id, falseExactCount);
    };

    run("zero_byte", {});
    run("non_adts", {'N', 'O', 'P', 'E', 0, 1, 2});
    std::vector<std::uint8_t> leading{'I', 'D', '3', 4, 0, 0, 0};
    append(leading, makeStream(base, 2));
    run("leading_id3", leading);
    std::vector<std::uint8_t> trailing = makeStream(base, 2);
    trailing.insert(trailing.end(), {'T', 'A', 'G', 0, 0, 0, 0});
    run("trailing_id3", trailing);

    for (const auto& item : std::array<std::pair<const char*, FrameConfig>, 4>{{
             {"unsupported_profile", FrameConfig{0, 0, 4, 2, true, 0, 23, 0x55}},
             {"crc_protected", FrameConfig{0, 1, 4, 2, false, 0, 23, 0x55}},
             {"multiple_raw_blocks", FrameConfig{0, 1, 4, 2, true, 1, 23, 0x55}},
             {"channel_config_zero", FrameConfig{0, 1, 4, 0, true, 0, 23, 0x55}},
         }}) {
        run(item.first, makeStream(item.second, 2));
    }

    auto mutateSecond = [&](const std::string& id, FrameConfig second) {
        std::vector<std::uint8_t> bytes = makeFrame(base);
        append(bytes, makeFrame(second));
        run(id, bytes);
    };
    FrameConfig changed = base;
    changed.mpegId = 1;
    mutateSecond("mpeg_id_change", changed);
    changed = base;
    changed.profileBits = 0;
    mutateSecond("profile_change", changed);
    changed = base;
    changed.sampleRateIndex = 3;
    mutateSecond("sample_rate_change", changed);
    changed = base;
    changed.channels = 1;
    mutateSecond("channel_change", changed);
    changed = base;
    changed.protectionAbsent = false;
    mutateSecond("protection_change", changed);
    changed = base;
    changed.rawDataBlockField = 1;
    mutateSecond("raw_block_change", changed);

    std::vector<std::uint8_t> middleJunk = makeFrame(base);
    middleJunk.insert(middleJunk.end(), {1, 2, 3, 4, 5, 6, 7});
    append(middleJunk, makeFrame(base));
    run("junk_between", middleJunk);
    std::vector<std::uint8_t> tailJunk = makeStream(base, 2);
    tailJunk.insert(tailJunk.end(), {1, 2, 3});
    run("junk_after", tailJunk);

    std::vector<std::uint8_t> badLength = makeFrame(base);
    badLength[3] &= 0xfc;
    badLength[4] = 0;
    badLength[5] = static_cast<std::uint8_t>((6U << 5U) | 0x1fU);
    run("frame_length_below_header", badLength);
    std::vector<std::uint8_t> beyond = makeFrame(base);
    beyond[3] = static_cast<std::uint8_t>((beyond[3] & 0xfcU) | 1U);
    run("frame_length_beyond_eof", beyond);
    std::vector<std::uint8_t> truncatedHeader = makeFrame(base);
    truncatedHeader.insert(truncatedHeader.end(), {0xff, 0xf1, 0x50});
    run("truncated_final_header", truncatedHeader);
    std::vector<std::uint8_t> truncatedPayload = makeFrame(base);
    truncatedPayload.resize(truncatedPayload.size() - 3);
    run("truncated_final_payload", truncatedPayload);
    FrameConfig forbidden = base;
    forbidden.sampleRateIndex = 15;
    run("forbidden_sample_rate", makeFrame(forbidden));

    const auto errorPath = root / L"read_error.aac";
    ok &= writeBytes(errorPath, makeStream(base, 20));
    Probe::AdtsAacSequentialTestHooks errorHooks;
    errorHooks.forceReadErrorAfterBytes = 64;
    ok &= expectNonExact(
        scan(errorPath, 44100, 2, 31, &errorHooks), "forced_read_error", falseExactCount);

    Probe::AdtsAacSequentialTestHooks overflowHooks;
    overflowHooks.initialFrameCount = (std::numeric_limits<std::uint64_t>::max)();
    ok &= expectNonExact(
        scan(errorPath, 44100, 2, 31, &overflowHooks), "counter_overflow", falseExactCount);
    Probe::AdtsAacSequentialTestHooks presentationOverflowHooks;
    presentationOverflowHooks.initialRawDataBlockCount =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) / 1024ULL;
    ok &= expectNonExact(
        scan(errorPath, 44100, 2, 31, &presentationOverflowHooks),
        "presentation_overflow", falseExactCount);
    Probe::AdtsAacSequentialTestHooks mutationHooks;
    mutationHooks.forceIdentityMismatchAtEnd = true;
    ok &= expectNonExact(
        scan(errorPath, 44100, 2, 31, &mutationHooks),
        "file_mutation", falseExactCount);
    return ok;
}

bool runEligibilityMatrix(const std::filesystem::path& validPath) {
    AVInputFormat input{};
    input.name = "aac";
    AVCodecParameters codec{};
    codec.codec_type = AVMEDIA_TYPE_AUDIO;
    codec.codec_id = AV_CODEC_ID_AAC;
    codec.profile = AV_PROFILE_AAC_LOW;
    codec.sample_rate = 44100;
    codec.frame_size = 1024;
    av_channel_layout_default(&codec.ch_layout, 2);
    AVStream stream{};
    stream.index = 0;
    stream.codecpar = &codec;
    AVStream* streams[]{&stream};
    AVFormatContext format{};
    format.iformat = &input;
    format.nb_streams = 1;
    format.streams = streams;

    bool ok = expect(
        Probe::evaluateAdtsAacSequentialEligibility(
            validPath.u8string(), &format, &stream, false).eligible,
        "eligibility_supported");
    input.name = "mov,mp4,m4a,3gp,3g2,mj2";
    ok &= expect(!Probe::evaluateAdtsAacSequentialEligibility(
        validPath.u8string(), &format, &stream, false).eligible, "mp4_excluded");
    input.name = "mpegts";
    ok &= expect(!Probe::evaluateAdtsAacSequentialEligibility(
        validPath.u8string(), &format, &stream, false).eligible, "mpegts_excluded");
    input.name = "matroska,webm";
    ok &= expect(!Probe::evaluateAdtsAacSequentialEligibility(
        validPath.u8string(), &format, &stream, false).eligible, "matroska_excluded");
    input.name = "aac";
    codec.profile = AV_PROFILE_AAC_HE;
    ok &= expect(!Probe::evaluateAdtsAacSequentialEligibility(
        validPath.u8string(), &format, &stream, false).eligible, "he_aac_excluded");
    codec.profile = AV_PROFILE_AAC_HE_V2;
    ok &= expect(!Probe::evaluateAdtsAacSequentialEligibility(
        validPath.u8string(), &format, &stream, false).eligible, "he_aac_v2_excluded");
    codec.profile = AV_PROFILE_AAC_LOW;
    codec.frame_size = 960;
    ok &= expect(!Probe::evaluateAdtsAacSequentialEligibility(
        validPath.u8string(), &format, &stream, false).eligible, "frame_size_excluded");
    codec.frame_size = 1024;
    ok &= expect(!Probe::evaluateAdtsAacSequentialEligibility(
        validPath.u8string(), &format, &stream, true).eligible, "stronger_authority_preserved");
    av_channel_layout_uninit(&codec.ch_layout);
    return ok;
}

bool runProductFallbackMatrix(
    const std::filesystem::path& root,
    const std::filesystem::path& shortAdts,
    const std::filesystem::path& longAdts) {
    auto probeFallback = [&](const std::string& id,
                             const std::vector<std::uint8_t>& bytes,
                             std::uint64_t expectedFrames,
                             bool sequentialEntered,
                             bool possibleDoublePass) {
        const std::filesystem::path media = root / (id + ".aac");
        const std::filesystem::path probePath = root / (id + ".json");
        bool ok = expect(writeBytes(media, bytes), id + "_write");
        ok &= expect(AveMediaBridge_ProbeToJson(media.c_str(), probePath.c_str()) == 0,
            id + "_probe");
        const std::string probe = readText(probePath);
        ok &= expect(jsonUnsigned(probe, "decodedSampleFrames") == expectedFrames,
            id + "_exact_loading");
        ok &= expect(jsonString(probe, "decodedSampleFramesSource") ==
            "aac_adts_packet_duration_sum", id + "_existing_source");
        ok &= expect(jsonBool(probe, "adtsAacSequentialEntered") == sequentialEntered,
            id + "_sequential_entered");
        ok &= expect(jsonBool(probe, "adtsAacSequentialGenericScanEntered") == true,
            id + "_generic_scan_entered");
        ok &= expect(jsonBool(probe, "adtsAacSequentialGenericScanSkipped") == false,
            id + "_generic_scan_not_skipped");
        ok &= expect(jsonBool(probe, "adtsAacSequentialPossibleDoublePass") ==
            possibleDoublePass, id + "_double_pass_diagnostic");
        return ok;
    };

    const std::vector<char> rawShort = readBytes(shortAdts);
    const std::vector<std::uint8_t> shortBytes(rawShort.begin(), rawShort.end());
    std::vector<std::uint8_t> leadingId3{
        'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0,
    };
    append(leadingId3, shortBytes);
    bool ok = probeFallback(
        "product_early_tag_fallback",
        leadingId3,
        12 * 1024ULL,
        true,
        false);

    const std::vector<char> rawLong = readBytes(longAdts);
    std::vector<std::uint8_t> changedConfiguration(rawLong.begin(), rawLong.end());
    if (changedConfiguration.size() < 14) {
        return expect(false, "product_late_configuration_fixture_size");
    }
    const std::size_t firstFrameBytes =
        ((static_cast<std::size_t>(changedConfiguration[3]) & 3U) << 11U) |
        (static_cast<std::size_t>(changedConfiguration[4]) << 3U) |
        (static_cast<std::size_t>(changedConfiguration[5]) >> 5U);
    if (firstFrameBytes + 7 > changedConfiguration.size()) {
        return expect(false, "product_late_configuration_first_frame");
    }
    changedConfiguration[firstFrameBytes + 2] &= 0xfeU;
    changedConfiguration[firstFrameBytes + 3] = static_cast<std::uint8_t>(
        (changedConfiguration[firstFrameBytes + 3] & 0x3fU) | 0x40U);
    ok &= probeFallback(
        "product_late_configuration_fallback",
        changedConfiguration,
        105841664ULL,
        true,
        true);
    return ok;
}

bool runRealProbe(
    const std::filesystem::path& media,
    std::uint64_t expectedFrames,
    bool golden) {
    ScopedTempDirectory temp(golden ? L"adts_golden_probe" : L"adts_real_probe");
    const auto probePath = temp.path() / L"probe.json";
    bool ok = expect(
        AveMediaBridge_ProbeToJson(media.c_str(), probePath.c_str()) == 0,
        golden ? "golden_probe" : "real_probe");
    const std::string probe = readText(probePath);
    const std::string caseId = media.filename().u8string();
    ok &= expect(jsonUnsigned(probe, "decodedSampleFrames") == expectedFrames,
        golden ? "golden_loading" : "real_loading", caseId);
    ok &= expect(jsonString(probe, "decodedSampleFramesSource") ==
        "adts_aac_sequential_presentation", "real_source",
        caseId + ":" + jsonString(probe, "decodedSampleFramesSource").value_or("missing") +
            ":" + jsonString(probe, "adtsAacSequentialReason").value_or("missing"));
    ok &= expect(jsonBool(probe, "adtsAacSequentialGenericScanEntered") == false,
        "real_generic_scan_not_entered", caseId);
    ok &= expect(jsonBool(probe, "adtsAacSequentialGenericScanSkipped") == true,
        "real_generic_scan_skipped", caseId);
    if (golden) {
        ok &= expect(jsonUnsigned(probe, "adtsAacSequentialFrameCount") == 103361,
            "golden_frame_count");
        ok &= expect(jsonUnsigned(probe, "adtsAacSequentialBytesReturned") == 40147651,
            "golden_bytes");
        ok &= expect(jsonUnsigned(probe, "adtsAacSequentialDuplicateBytes") == 0,
            "golden_duplicate_bytes");
    }
    return ok;
}

bool runProbeHandoffIntegration(
    const std::filesystem::path& media,
    std::uint64_t expectedFrames) {
    ScopedTempDirectory temp(L"adts_handoff");
    const auto exactSession = temp.path() / L"exact";
    const auto fallbackSession = temp.path() / L"fallback";
    const auto staleSession = temp.path() / L"stale";
    std::error_code error;
    std::filesystem::create_directories(exactSession, error);
    std::filesystem::create_directories(fallbackSession, error);
    std::filesystem::create_directories(staleSession, error);
    bool ok = expect(!error, "integration_dirs", error.message());

    const auto probePath = exactSession / L"probe.json";
    ok &= expect(AveMediaBridge_ProbeToJson(media.c_str(), probePath.c_str()) == 0,
        "integration_probe");
    const std::string probe = readText(probePath);
    ok &= expect(writeText(staleSession / L"probe.json", probe), "stale_probe_copy");
    std::string staleProbe = readText(staleSession / L"probe.json");
    const std::string token = "\"adtsAacSequentialFileSizeBytes\": ";
    const std::size_t offset = staleProbe.find(token);
    ok &= expect(offset != std::string::npos, "stale_identity_field");
    if (offset != std::string::npos) {
        const std::size_t begin = offset + token.size();
        const std::size_t end = staleProbe.find(',', begin);
        const auto size = jsonUnsigned(staleProbe, "adtsAacSequentialFileSizeBytes");
        if (end != std::string::npos && size) {
            staleProbe.replace(begin, end - begin, std::to_string(*size + 1));
            ok &= expect(writeText(staleSession / L"probe.json", staleProbe),
                "stale_identity_write");
        }
    }

    auto import = [&](const std::filesystem::path& session, const char* id) {
        AveMediaBridgeImportOptions options{};
        options.structSize = sizeof(options);
        options.inputPath = media.c_str();
        options.sessionMediaDir = session.c_str();
        return expect(AveMediaBridge_ImportAudioToSessionEx(&options) == 0, id);
    };
    ok &= import(exactSession, "integration_exact_import");
    ok &= import(fallbackSession, "integration_fallback_import");
    ok &= import(staleSession, "integration_stale_import");

    const std::string exactInfo = readText(exactSession / L"audio_info.json");
    const std::string fallbackInfo = readText(fallbackSession / L"audio_info.json");
    const std::string staleInfo = readText(staleSession / L"audio_info.json");
    const std::string exactMetadata = readText(exactSession / L"metadata.json");
    const std::string fallbackMetadata = readText(fallbackSession / L"metadata.json");
    const std::string staleMetadata = readText(staleSession / L"metadata.json");
    ok &= expect(jsonUnsigned(exactInfo, "frames") == expectedFrames, "exact_ready");
    ok &= expect(jsonUnsigned(fallbackInfo, "frames") == expectedFrames, "fallback_ready");
    ok &= expect(jsonUnsigned(staleInfo, "frames") == expectedFrames, "stale_ready");
    ok &= expect(jsonString(exactMetadata, "source") ==
        "adts_aac_sequential_presentation", "budget_source");
    ok &= expect(jsonBool(exactMetadata, "packetScanExecuted") == false,
        "import_generic_scan_skipped");
    ok &= expect(jsonBool(exactMetadata, "adtsAacSequentialAuthorityReused") == true,
        "handoff_reused");
    ok &= expect(jsonBool(fallbackMetadata, "packetScanExecuted") == false,
        "fallback_import_does_not_add_scan");
    ok &= expect(jsonBool(staleMetadata, "packetScanExecuted") == false,
        "stale_import_does_not_add_scan");
    ok &= expect(jsonBool(staleMetadata, "adtsAacSequentialAuthorityReused") == false,
        "stale_handoff_rejected");
    const auto exactPcm = readBytes(exactSession / L"original_f32.bin");
    ok &= expect(exactPcm == readBytes(fallbackSession / L"original_f32.bin"),
        "fallback_pcm_parity");
    ok &= expect(exactPcm == readBytes(staleSession / L"original_f32.bin"),
        "stale_pcm_parity");
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 7) {
        std::cout << "AVEMEDIABRIDGE_ADTS_AAC_SEQUENTIAL_PRESENTATION_SKIPPED: "
                     "Golden and five real ADTS paths are required\n";
        return 77;
    }
    const std::filesystem::path golden = std::filesystem::u8path(argv[1]);
    const std::array<std::filesystem::path, 4> real{
        std::filesystem::u8path(argv[2]),
        std::filesystem::u8path(argv[3]),
        std::filesystem::u8path(argv[4]),
        std::filesystem::u8path(argv[5]),
    };
    const std::filesystem::path integration = std::filesystem::u8path(argv[6]);
    if (!std::filesystem::is_regular_file(golden) ||
        !std::all_of(real.begin(), real.end(), [](const auto& path) {
            return std::filesystem::is_regular_file(path);
        }) || !std::filesystem::is_regular_file(integration)) {
        std::cout << "AVEMEDIABRIDGE_ADTS_AAC_SEQUENTIAL_PRESENTATION_SKIPPED: "
                     "required media unavailable\n";
        return 77;
    }

    ScopedTempDirectory generated(L"adts_sequential_matrix");
    bool ok = runValidMatrix(generated.path());
    ok &= runInventorySemantics(generated.path());
    std::size_t falseExactCount = 0;
    ok &= runRobustnessMatrix(generated.path(), falseExactCount);
    ok &= runEligibilityMatrix(generated.path() / L"mpeg4_44100_stereo.aac");
    ok &= runProductFallbackMatrix(generated.path(), integration, golden);
    ok &= runRealProbe(golden, 105841664, true);
    const std::array<std::uint64_t, 4> expected{12288, 2048, 2048, 103424};
    for (std::size_t index = 0; index < real.size(); ++index) {
        ok &= runRealProbe(real[index], expected[index], false);
    }
    ok &= runProbeHandoffIntegration(integration, 12288);
    ok &= expect(falseExactCount == 0, "false_exact_count");
    if (!ok) {
        return 1;
    }
    std::cout
        << "AVEMEDIABRIDGE_ADTS_AAC_SEQUENTIAL_PRESENTATION_AUTHORITY_OK "
        << "validExactCases=22 nonExactCases=34 "
        << "falseExactCount=" << falseExactCount
        << " genericHealthyScanEntered=no commonHealthyDoublePass=no\n";
    return 0;
}
