#include "AveMediaBridge/AveMediaBridgeApi.hpp"
#include "Probe/Ac3Eac3SequentialPresentation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
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
        ? std::string(std::istreambuf_iterator<char>(input),
              std::istreambuf_iterator<char>())
        : std::string{};
}

std::vector<char> readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return input
        ? std::vector<char>(std::istreambuf_iterator<char>(input),
              std::istreambuf_iterator<char>())
        : std::vector<char>{};
}

bool writeBytes(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
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

void setField(
    std::array<std::uint8_t, 7>& bytes,
    unsigned firstBit,
    unsigned count,
    std::uint32_t value) {
    for (unsigned index = 0; index < count; ++index) {
        const unsigned bit = firstBit + index;
        const unsigned byteIndex = bit / 8;
        const unsigned bitInByte = 7 - (bit % 8);
        const std::uint8_t mask = static_cast<std::uint8_t>(1U << bitInByte);
        const bool set = (value & (1U << (count - index - 1))) != 0;
        bytes[byteIndex] = set
            ? static_cast<std::uint8_t>(bytes[byteIndex] | mask)
            : static_cast<std::uint8_t>(bytes[byteIndex] & ~mask);
    }
}

constexpr std::array<std::array<std::uint16_t, 3>, 38> kAc3FrameSizeWords{{
    {{64, 69, 96}}, {{64, 70, 96}}, {{80, 87, 120}}, {{80, 88, 120}},
    {{96, 104, 144}}, {{96, 105, 144}}, {{112, 121, 168}}, {{112, 122, 168}},
    {{128, 139, 192}}, {{128, 140, 192}}, {{160, 174, 240}}, {{160, 175, 240}},
    {{192, 208, 288}}, {{192, 209, 288}}, {{224, 243, 336}}, {{224, 244, 336}},
    {{256, 278, 384}}, {{256, 279, 384}}, {{320, 348, 480}}, {{320, 349, 480}},
    {{384, 417, 576}}, {{384, 418, 576}}, {{448, 487, 672}}, {{448, 488, 672}},
    {{512, 557, 768}}, {{512, 558, 768}}, {{640, 696, 960}}, {{640, 697, 960}},
    {{768, 835, 1152}}, {{768, 836, 1152}}, {{896, 975, 1344}}, {{896, 976, 1344}},
    {{1024, 1114, 1536}}, {{1024, 1115, 1536}}, {{1152, 1253, 1728}},
    {{1152, 1254, 1728}}, {{1280, 1393, 1920}}, {{1280, 1394, 1920}},
}};

std::vector<std::uint8_t> makeAc3Frame(
    int sampleRateCode,
    int frameSizeCode,
    int bitstreamId = 8,
    int channelMode = 2,
    bool lfe = false,
    std::uint8_t payload = 0x55) {
    const std::size_t frameBytes =
        static_cast<std::size_t>(kAc3FrameSizeWords[frameSizeCode][sampleRateCode]) * 2U;
    std::vector<std::uint8_t> result(frameBytes, payload);
    std::array<std::uint8_t, 7> header{};
    setField(header, 0, 16, 0x0b77);
    setField(header, 32, 2, static_cast<std::uint32_t>(sampleRateCode));
    setField(header, 34, 6, static_cast<std::uint32_t>(frameSizeCode));
    setField(header, 40, 5, static_cast<std::uint32_t>(bitstreamId));
    setField(header, 45, 3, 0);
    setField(header, 48, 3, static_cast<std::uint32_t>(channelMode));
    unsigned lfeBit = 51;
    if (channelMode == 2) {
        lfeBit += 2;
    } else {
        if ((channelMode & 1) && channelMode != 1) lfeBit += 2;
        if (channelMode & 4) lfeBit += 2;
    }
    setField(header, lfeBit, 1, lfe ? 1U : 0U);
    std::copy(header.begin(), header.end(), result.begin());
    return result;
}

std::vector<std::uint8_t> makeEac3Frame(
    int sampleRateCode,
    int blockCode,
    int frameBytes = 256,
    int streamType = 0,
    int substreamId = 0,
    int bitstreamId = 16,
    int channelMode = 2,
    bool lfe = false,
    std::uint8_t payload = 0x66) {
    frameBytes += frameBytes % 2;
    std::vector<std::uint8_t> result(static_cast<std::size_t>(frameBytes), payload);
    std::array<std::uint8_t, 7> header{};
    setField(header, 0, 16, 0x0b77);
    setField(header, 16, 2, static_cast<std::uint32_t>(streamType));
    setField(header, 18, 3, static_cast<std::uint32_t>(substreamId));
    setField(header, 21, 11, static_cast<std::uint32_t>(frameBytes / 2 - 1));
    setField(header, 32, 2, static_cast<std::uint32_t>(sampleRateCode));
    setField(header, 34, 2, static_cast<std::uint32_t>(blockCode));
    setField(header, 36, 3, static_cast<std::uint32_t>(channelMode));
    setField(header, 39, 1, lfe ? 1U : 0U);
    setField(header, 40, 5, static_cast<std::uint32_t>(bitstreamId));
    std::copy(header.begin(), header.end(), result.begin());
    return result;
}

void append(
    std::vector<std::uint8_t>& destination,
    const std::vector<std::uint8_t>& source) {
    destination.insert(destination.end(), source.begin(), source.end());
}

Probe::DolbySelectedStreamIdentity selected(
    AVCodecID codec,
    int sampleRate,
    int channels = 2) {
    Probe::DolbySelectedStreamIdentity result;
    result.streamIndex = 0;
    result.codecId = codec;
    result.sampleRate = sampleRate;
    result.channels = channels;
    return result;
}

Probe::DolbySequentialPresentationResult scan(
    const std::filesystem::path& path,
    AVCodecID codec,
    int sampleRate,
    int channels = 2,
    const Probe::DolbySequentialTestHooks* hooks = nullptr,
    std::size_t bufferBytes = Probe::kDolbySequentialReadBufferBytes) {
    Probe::DolbySequentialProbeOptions options;
    options.testHooks = hooks;
    options.readBufferBytes = bufferBytes;
    return Probe::probeDolbySequentialPresentation(
        path.u8string(), selected(codec, sampleRate, channels), options);
}

bool verifyExact(
    const Probe::DolbySequentialPresentationResult& result,
    const std::string& id,
    std::uint64_t expectedFrames,
    Probe::DolbySequentialCodecFamily family) {
    const Probe::TotalPresentationEvidence evidence =
        Probe::makeDolbySequentialTotalPresentationEvidence(result);
    const auto source = family == Probe::DolbySequentialCodecFamily::Ac3
        ? Probe::PresentationTotalSource::Ac3SequentialPresentation
        : Probe::PresentationTotalSource::Eac3SequentialPresentation;
    return expect(result.exact(), id, Probe::dolbySequentialReasonName(result.reason)) &&
        expect(result.family == family, id + "_family") &&
        expect(result.presentationFrames == expectedFrames, id + "_frames") &&
        expect(result.bytesReturned == result.fileSizeBytes, id + "_bytes") &&
        expect(result.uniqueBytes == result.fileSizeBytes, id + "_unique") &&
        expect(result.duplicateBytes == 0, id + "_duplicate") &&
        expect(result.seekCallsAfterOpen == 0, id + "_seek") &&
        expect(result.reachedPhysicalEof, id + "_eof") &&
        expect(result.frameBoundariesValid, id + "_boundaries") &&
        expect(result.configurationContinuous, id + "_configuration") &&
        expect(result.substreamPolicyValid, id + "_substream") &&
        expect(!result.crcValidated, id + "_crc_not_claimed") &&
        expect(result.payloadValiditySeparatedFromExtent, id + "_payload_domain") &&
        expect(evidence.frames == expectedFrames &&
                   evidence.trust == Probe::PresentationTotalTrust::SampleExact &&
                   evidence.source == source &&
                   evidence.domain == Probe::PresentationSampleDomain::NativeStreamSamples &&
                   evidence.validation == Probe::PresentationTotalValidation::SelfContainedMetadata,
            id + "_evidence");
}

bool runExactMatrix(const std::filesystem::path& root) {
    bool ok = true;
    std::size_t caseIndex = 0;
    for (int sampleRateCode = 0; sampleRateCode < 3; ++sampleRateCode) {
        std::vector<std::uint8_t> bytes;
        for (int index = 0; index < 4; ++index) {
            append(bytes, makeAc3Frame(sampleRateCode, 10 + index, 8, 2, false,
                static_cast<std::uint8_t>(0x30 + index)));
        }
        const auto path = root / (L"exact_ac3_" + std::to_wstring(caseIndex++) + L".ac3");
        ok &= expect(writeBytes(path, bytes), "write_exact_ac3");
        const int rate = std::array<int, 3>{48000, 44100, 32000}[sampleRateCode];
        ok &= verifyExact(scan(path, AV_CODEC_ID_AC3, rate, 2, nullptr, 17),
            "exact_ac3_" + std::to_string(sampleRateCode), 4 * 1536,
            Probe::DolbySequentialCodecFamily::Ac3);
    }
    constexpr std::array<int, 4> blocks{1, 2, 3, 6};
    for (int sampleRateCode = 0; sampleRateCode < 3; ++sampleRateCode) {
        std::vector<std::uint8_t> bytes;
        std::uint64_t expectedBlocks = 0;
        for (int blockCode = 0; blockCode < 4; ++blockCode) {
            append(bytes, makeEac3Frame(sampleRateCode, blockCode,
                210 + blockCode * 18, 0, 0, 16));
            expectedBlocks += blocks[blockCode];
        }
        const auto path = root / (L"exact_eac3_" + std::to_wstring(caseIndex++) + L".eac3");
        ok &= expect(writeBytes(path, bytes), "write_exact_eac3");
        const int rate = std::array<int, 3>{48000, 44100, 32000}[sampleRateCode];
        ok &= verifyExact(scan(path, AV_CODEC_ID_EAC3, rate, 2, nullptr, 19),
            "exact_eac3_" + std::to_string(sampleRateCode), expectedBlocks * 256,
            Probe::DolbySequentialCodecFamily::Eac3);
    }

    const auto mono = root / L"exact_mono.ac3";
    ok &= expect(writeBytes(mono, makeAc3Frame(0, 0, 8, 1)), "write_mono");
    ok &= verifyExact(scan(mono, AV_CODEC_ID_AC3, 48000, 1),
        "exact_mono", 1536, Probe::DolbySequentialCodecFamily::Ac3);
    const auto multichannel = root / L"exact_multichannel.eac3";
    ok &= expect(writeBytes(multichannel, makeEac3Frame(0, 3, 300, 0, 0, 16, 7, true)),
        "write_multichannel");
    ok &= verifyExact(scan(multichannel, AV_CODEC_ID_EAC3, 48000, 6),
        "exact_multichannel", 1536, Probe::DolbySequentialCodecFamily::Eac3);
    return ok;
}

bool runRobustnessMatrix(
    const std::filesystem::path& root,
    std::size_t& falseExactCount) {
    bool ok = true;
    auto nonExact = [&](const std::string& id,
                        const std::vector<std::uint8_t>& bytes,
                        AVCodecID codec,
                        int rate,
                        const Probe::DolbySequentialTestHooks* hooks = nullptr) {
        const auto path = root / std::filesystem::u8path(id +
            (codec == AV_CODEC_ID_AC3 ? ".ac3" : ".eac3"));
        ok &= expect(writeBytes(path, bytes), id + "_write");
        const auto result = scan(path, codec, rate, 2, hooks, 13);
        if (result.exact()) {
            ++falseExactCount;
        }
        ok &= expect(!result.exact(), id,
            Probe::dolbySequentialReasonName(result.reason));
        return result;
    };

    nonExact("empty", {}, AV_CODEC_ID_AC3, 48000);
    nonExact("truncated_header", {0x0b, 0x77, 0, 0}, AV_CODEC_ID_AC3, 48000);
    auto truncatedFrame = makeAc3Frame(0, 0);
    truncatedFrame.pop_back();
    nonExact("truncated_frame", truncatedFrame, AV_CODEC_ID_AC3, 48000);
    auto junkHead = makeAc3Frame(0, 0);
    junkHead.insert(junkHead.begin(), 0x44);
    nonExact("junk_head", junkHead, AV_CODEC_ID_AC3, 48000);
    auto junkMiddle = makeAc3Frame(0, 0);
    junkMiddle.push_back(0x44);
    append(junkMiddle, makeAc3Frame(0, 0));
    nonExact("junk_middle", junkMiddle, AV_CODEC_ID_AC3, 48000);
    auto junkTail = makeAc3Frame(0, 0);
    junkTail.push_back(0x44);
    nonExact("junk_tail", junkTail, AV_CODEC_ID_AC3, 48000);
    nonExact("unsupported_bsid", makeAc3Frame(0, 0, 9), AV_CODEC_ID_AC3, 48000);
    auto mixed = makeAc3Frame(0, 0);
    append(mixed, makeEac3Frame(0, 3));
    nonExact("mixed_family", mixed, AV_CODEC_ID_AC3, 48000);
    auto rateChange = makeAc3Frame(0, 0);
    append(rateChange, makeAc3Frame(1, 0));
    nonExact("rate_change", rateChange, AV_CODEC_ID_AC3, 48000);
    auto channelsChange = makeAc3Frame(0, 0);
    append(channelsChange, makeAc3Frame(0, 0, 8, 1));
    nonExact("channels_change", channelsChange, AV_CODEC_ID_AC3, 48000);
    auto bsidChange = makeEac3Frame(0, 3, 256, 0, 0, 11);
    append(bsidChange, makeEac3Frame(0, 3, 256, 0, 0, 16));
    nonExact("bsid_change", bsidChange, AV_CODEC_ID_EAC3, 48000);
    nonExact("reduced_rate", makeEac3Frame(3, 0), AV_CODEC_ID_EAC3, 48000);
    nonExact("dependent", makeEac3Frame(0, 3, 256, 1), AV_CODEC_ID_EAC3, 48000);
    nonExact("reserved_stream", makeEac3Frame(0, 3, 256, 3), AV_CODEC_ID_EAC3, 48000);
    nonExact("other_substream", makeEac3Frame(0, 3, 256, 0, 1), AV_CODEC_ID_EAC3, 48000);

    Probe::DolbySequentialTestHooks readError;
    readError.forceReadErrorAfterBytes = 1;
    nonExact("read_error", makeAc3Frame(0, 0), AV_CODEC_ID_AC3, 48000, &readError);
    Probe::DolbySequentialTestHooks overflow;
    overflow.initialSyncframeCount = (std::numeric_limits<std::uint64_t>::max)();
    nonExact("counter_overflow", makeAc3Frame(0, 0), AV_CODEC_ID_AC3, 48000, &overflow);
    Probe::DolbySequentialTestHooks blockOverflow;
    blockOverflow.initialAudioBlockCount = (std::numeric_limits<std::uint64_t>::max)();
    nonExact("block_overflow", makeEac3Frame(0, 3), AV_CODEC_ID_EAC3, 48000, &blockOverflow);
    Probe::DolbySequentialTestHooks identityMismatch;
    identityMismatch.forceIdentityMismatchAtEnd = true;
    nonExact("identity_mismatch", makeAc3Frame(0, 0), AV_CODEC_ID_AC3, 48000,
        &identityMismatch);

    auto fakeSyncPayload = makeAc3Frame(0, 0);
    fakeSyncPayload[20] = 0x0b;
    fakeSyncPayload[21] = 0x77;
    const auto fakePath = root / L"fake_sync_payload.ac3";
    ok &= expect(writeBytes(fakePath, fakeSyncPayload), "fake_sync_write");
    ok &= verifyExact(scan(fakePath, AV_CODEC_ID_AC3, 48000),
        "fake_sync_payload", 1536, Probe::DolbySequentialCodecFamily::Ac3);

    auto inventory = makeAc3Frame(0, 0);
    append(inventory, makeAc3Frame(0, 0));
    const auto twoPath = root / L"inventory_two.ac3";
    ok &= expect(writeBytes(twoPath, inventory), "inventory_two_write");
    ok &= verifyExact(scan(twoPath, AV_CODEC_ID_AC3, 48000),
        "inventory_two", 3072, Probe::DolbySequentialCodecFamily::Ac3);
    const auto onePath = root / L"inventory_one.ac3";
    ok &= expect(writeBytes(onePath, makeAc3Frame(0, 0)), "inventory_one_write");
    ok &= verifyExact(scan(onePath, AV_CODEC_ID_AC3, 48000),
        "inventory_deleted", 1536, Probe::DolbySequentialCodecFamily::Ac3);
    auto three = inventory;
    append(three, makeAc3Frame(0, 0));
    const auto threePath = root / L"inventory_three.ac3";
    ok &= expect(writeBytes(threePath, three), "inventory_three_write");
    ok &= verifyExact(scan(threePath, AV_CODEC_ID_AC3, 48000),
        "inventory_duplicated", 4608, Probe::DolbySequentialCodecFamily::Ac3);
    return ok;
}

bool runRealProbe(
    const std::filesystem::path& media,
    std::uint64_t expectedFrames,
    const std::string& expectedSource) {
    ScopedTempDirectory temp(L"dolby_real_probe");
    const auto probePath = temp.path() / L"probe.json";
    bool ok = expect(AveMediaBridge_ProbeToJson(media.c_str(), probePath.c_str()) == 0,
        "real_probe", media.u8string());
    const std::string probe = readText(probePath);
    ok &= expect(jsonUnsigned(probe, "decodedSampleFrames") == expectedFrames,
        "real_loading", media.filename().u8string());
    ok &= expect(jsonString(probe, "decodedSampleFramesSource") == expectedSource,
        "real_source", media.filename().u8string());
    ok &= expect(jsonBool(probe, "dolbySequentialTypedEvidencePublished") == true,
        "real_typed_evidence");
    ok &= expect(jsonBool(probe, "dolbySequentialGenericFullScanEntered") == false,
        "real_fast_generic_scan");
    ok &= expect(jsonBool(probe, "dolbySequentialGenericFullScanSkipped") == false,
        "real_no_artificial_skip_flag");
    ok &= expect(jsonUnsigned(probe, "dolbySequentialBytesReturned") ==
        std::filesystem::file_size(media), "real_bytes");
    ok &= expect(jsonUnsigned(probe, "dolbySequentialDuplicateBytes") == 0,
        "real_duplicate_bytes");
    ok &= expect(jsonUnsigned(probe, "dolbySequentialSeekCallsAfterOpen") == 0,
        "real_seek_calls");
    return ok;
}

bool runContainerizedControl(const std::filesystem::path& media) {
    ScopedTempDirectory temp(L"dolby_container_control");
    const auto probePath = temp.path() / L"probe.json";
    bool ok = expect(AveMediaBridge_ProbeToJson(media.c_str(), probePath.c_str()) == 0,
        "containerized_probe");
    const std::string probe = readText(probePath);
    ok &= expect(jsonBool(probe, "dolbySequentialEligible") == false,
        "containerized_not_eligible");
    ok &= expect(jsonBool(probe, "dolbySequentialEntered") == false,
        "containerized_not_entered");
    ok &= expect(jsonString(probe, "dolbySequentialReason") ==
        "not_standalone_raw_dolby", "containerized_reason");
    return ok;
}

bool runEligibility(const std::filesystem::path& media) {
    OpenedMedia opened;
    bool ok = expect(openSelected(media, opened), "eligibility_open");
    if (!ok) {
        return false;
    }
    const auto eligible = Probe::evaluateDolbySequentialEligibility(
        media.u8string(), opened.context, opened.selected, false);
    ok &= expect(eligible.eligible, "eligibility_exact");
    ok &= expect(!Probe::evaluateDolbySequentialEligibility(
        media.u8string(), opened.context, opened.selected, true).eligible,
        "eligibility_stronger_authority");
    AVCodecParameters copied = *opened.selected->codecpar;
    AVStream fake = *opened.selected;
    fake.codecpar = &copied;
    copied.codec_id = copied.codec_id == AV_CODEC_ID_AC3
        ? AV_CODEC_ID_EAC3 : AV_CODEC_ID_AC3;
    ok &= expect(!Probe::evaluateDolbySequentialEligibility(
        media.u8string(), opened.context, &fake, false).eligible,
        "eligibility_codec_mismatch");
    return ok;
}

bool importSession(
    const std::filesystem::path& media,
    const std::filesystem::path& session) {
    AveMediaBridgeImportOptions options{};
    options.structSize = sizeof(options);
    options.inputPath = media.c_str();
    options.sessionMediaDir = session.c_str();
    return AveMediaBridge_ImportAudioToSessionEx(&options) == 0;
}

bool runHandoffIntegration(
    const std::filesystem::path& media,
    std::uint64_t expectedFrames,
    const std::string& expectedSource,
    const std::string& id) {
    ScopedTempDirectory temp(L"dolby_handoff");
    const auto exactSession = temp.path() / L"exact";
    const auto fallbackSession = temp.path() / L"fallback";
    std::error_code error;
    std::filesystem::create_directories(exactSession, error);
    std::filesystem::create_directories(fallbackSession, error);
    bool ok = expect(!error, id + "_dirs", error.message());
    ok &= expect(AveMediaBridge_ProbeToJson(
        media.c_str(), (exactSession / L"probe.json").c_str()) == 0,
        id + "_probe");
    ok &= expect(importSession(media, exactSession), id + "_exact_import");
    ok &= expect(importSession(media, fallbackSession), id + "_fallback_import");

    const std::string exactInfo = readText(exactSession / L"audio_info.json");
    const std::string fallbackInfo = readText(fallbackSession / L"audio_info.json");
    const std::string exactMetadata = readText(exactSession / L"metadata.json");
    const std::string fallbackMetadata = readText(fallbackSession / L"metadata.json");
    ok &= expect(jsonUnsigned(exactInfo, "frames") == expectedFrames,
        id + "_exact_ready");
    ok &= expect(jsonUnsigned(fallbackInfo, "frames") == expectedFrames,
        id + "_fallback_ready");
    ok &= expect(jsonString(exactMetadata, "source") == expectedSource,
        id + "_budget_source");
    ok &= expect(jsonBool(exactMetadata, "packetScanExecuted") == false,
        id + "_packet_scan_skipped");
    ok &= expect(jsonBool(exactMetadata, "dolbySequentialAuthorityReused") == true,
        id + "_handoff_reused");
    ok &= expect(jsonBool(exactMetadata, "genericDolbyFullPacketScanEntered") == false,
        id + "_generic_scan_not_entered");
    ok &= expect(jsonBool(exactMetadata, "dolbyDuplicatePresentationScanEntered") == false,
        id + "_no_duplicate_scan");
    ok &= expect(jsonBool(fallbackMetadata, "packetScanExecuted") == false,
        id + "_fallback_current_no_scan");
    const auto exactPcm = readBytes(exactSession / L"original_f32.bin");
    const auto fallbackPcm = readBytes(fallbackSession / L"original_f32.bin");
    ok &= expect(!exactPcm.empty(), id + "_pcm_nonempty");
    ok &= expect(exactPcm == fallbackPcm, id + "_pcm_parity");
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 8) {
        std::cout << "AVEMEDIABRIDGE_AC3_EAC3_SEQUENTIAL_PRESENTATION_SKIPPED: "
                     "six raw Dolby paths are required\n";
        return 77;
    }
    const std::array<std::filesystem::path, 6> media{
        std::filesystem::u8path(argv[1]),
        std::filesystem::u8path(argv[2]),
        std::filesystem::u8path(argv[3]),
        std::filesystem::u8path(argv[4]),
        std::filesystem::u8path(argv[5]),
        std::filesystem::u8path(argv[6]),
    };
    const std::filesystem::path containerized = std::filesystem::u8path(argv[7]);
    if (!std::all_of(media.begin(), media.end(), [](const auto& path) {
            return std::filesystem::is_regular_file(path);
        }) || !std::filesystem::is_regular_file(containerized)) {
        std::cout << "AVEMEDIABRIDGE_AC3_EAC3_SEQUENTIAL_PRESENTATION_SKIPPED: "
                     "required media unavailable\n";
        return 77;
    }

    ScopedTempDirectory generated(L"dolby_sequential_matrix");
    bool ok = runExactMatrix(generated.path());
    std::size_t falseExactCount = 0;
    ok &= runRobustnessMatrix(generated.path(), falseExactCount);
    ok &= runEligibility(media[0]);
    const std::array<std::uint64_t, 6> expected{
        44544, 101376, 32256, 1536, 1536, 101376,
    };
    const std::array<std::string, 6> sources{
        "eac3_sequential_presentation",
        "ac3_sequential_presentation",
        "ac3_sequential_presentation",
        "ac3_sequential_presentation",
        "eac3_sequential_presentation",
        "eac3_sequential_presentation",
    };
    for (std::size_t index = 0; index < media.size(); ++index) {
        ok &= runRealProbe(media[index], expected[index], sources[index]);
    }
    ok &= runContainerizedControl(containerized);
    ok &= runHandoffIntegration(media[0], expected[0], sources[0], "ea053");
    ok &= runHandoffIntegration(media[1], expected[1], sources[1], "ea142");
    ok &= expect(falseExactCount == 0, "false_exact_count");
    if (!ok) {
        return 1;
    }
    std::cout
        << "AVEMEDIABRIDGE_AC3_EAC3_SEQUENTIAL_PRESENTATION_AUTHORITY_OK "
        << "validExactCases=14 nonExactCases=19 falseExactCount="
        << falseExactCount
        << " genericHealthyScanEntered=no commonHealthyDoublePass=no\n";
    return 0;
}
