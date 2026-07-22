#include "AveMediaBridge/AveMediaBridgeApi.hpp"
#include "Probe/Mp4Mp3SampleEditTablePresentation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            (L"avemediabridge_mp4_mp3_tables_" + std::to_wstring(ticks));
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

struct OpenedMedia {
    AVFormatContext* context = nullptr;
    AVStream* selected = nullptr;

    ~OpenedMedia() {
        if (context) {
            avformat_close_input(&context);
        }
    }
};

bool openAudioOrdinal(
    const std::filesystem::path& path,
    int audioOrdinal,
    OpenedMedia& media) {
    if (avformat_open_input(
            &media.context, path.u8string().c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(media.context, nullptr) < 0) {
        return false;
    }
    int ordinal = 0;
    for (unsigned index = 0; index < media.context->nb_streams; ++index) {
        AVStream* stream = media.context->streams[index];
        if (!stream || !stream->codecpar ||
            stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        if (ordinal++ == audioOrdinal) {
            media.selected = stream;
            return true;
        }
    }
    return false;
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return input
        ? std::vector<std::uint8_t>(
              std::istreambuf_iterator<char>(input),
              std::istreambuf_iterator<char>())
        : std::vector<std::uint8_t>{};
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return input
        ? std::string(std::istreambuf_iterator<char>(input),
                      std::istreambuf_iterator<char>())
        : std::string{};
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

std::size_t findBoxType(
    const std::vector<std::uint8_t>& bytes,
    const std::array<char, 4>& type) {
    for (std::size_t offset = 4; offset + 4 <= bytes.size(); ++offset) {
        if (std::equal(type.begin(), type.end(), bytes.begin() + offset)) {
            return offset;
        }
    }
    return std::string::npos;
}

std::uint32_t readBe32(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
        static_cast<std::uint32_t>(bytes[offset + 3]);
}

void writeBe32(
    std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 24);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<std::uint8_t>(value);
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

struct ExactCase {
    const char* name;
    const char* file;
    int audioOrdinal;
    std::uint64_t presentation;
    std::uint64_t physical;
    std::uint64_t samplesPerFrame;
};

bool validateExactCase(
    const std::filesystem::path& path,
    int audioOrdinal,
    std::uint64_t expectedPresentation,
    std::uint64_t expectedPhysical,
    std::uint64_t expectedSamplesPerFrame,
    const std::string& id) {
    OpenedMedia media;
    if (!openAudioOrdinal(path, audioOrdinal, media)) {
        return expect(false, id, "FFmpeg could not open selected audio stream");
    }
    const auto eligibility = Probe::evaluateMp4Mp3SampleTableEligibility(
        path.u8string(), media.context, media.selected, false);
    bool ok = expect(eligibility.eligible, id, "eligibility rejected");
    if (!eligibility.eligible) {
        return false;
    }
    const auto result = Probe::probeMp4Mp3SampleEditTablePresentation(
        path.u8string(), eligibility);
    ok &= expect(result.exact(), id, Probe::mp4Mp3SampleTableReasonName(result.reason));
    ok &= expect(result.presentationFrames == expectedPresentation, id, "presentation mismatch");
    ok &= expect(result.physicalFrames == expectedPhysical, id, "physical total mismatch");
    ok &= expect(result.samplesPerMp3Frame == expectedSamplesPerFrame, id, "MPEG frame duration mismatch");
    ok &= expect(result.mp3FramesPerMp4Sample == 1, id, "MP4 sample is not one MP3 frame");
    ok &= expect(result.sampleInventoryValid && result.chunkMappingValid &&
                 result.chunkRangesInsideMdat && result.editListValid &&
                 result.mp3ProfileValid && result.checkedArithmeticValid,
                 id, "proof flags incomplete");
    ok &= expect(result.bytesReturned <= result.hardReadBudgetBytes &&
                 result.maximumBudgetOverrunBytes == 0,
                 id, "hard read budget exceeded");
    ok &= expect(Probe::mp4Mp3SampleTableMatchesStream(result, media.selected),
                 id, "selected stream conflict");
    const auto evidence =
        Probe::makeMp4Mp3SampleTableTotalPresentationEvidence(result);
    ok &= expect(evidence.trust == Probe::PresentationTotalTrust::SampleExact &&
                 evidence.source == Probe::PresentationTotalSource::
                     Mp4Mp3SampleEditTablePresentation &&
                 evidence.domain == Probe::PresentationSampleDomain::NativeStreamSamples &&
                 evidence.validation == Probe::PresentationTotalValidation::SelfContainedMetadata,
                 id, "typed evidence mismatch");
    return ok;
}

bool validateNonExact(
    const std::filesystem::path& path,
    int audioOrdinal,
    const std::string& id,
    int& falseExactCount) {
    OpenedMedia media;
    if (!openAudioOrdinal(path, audioOrdinal, media)) {
        return expect(false, id, "FFmpeg could not inspect fallback fixture");
    }
    const auto eligibility = Probe::evaluateMp4Mp3SampleTableEligibility(
        path.u8string(), media.context, media.selected, false);
    if (!eligibility.eligible) {
        return true;
    }
    const auto result = Probe::probeMp4Mp3SampleEditTablePresentation(
        path.u8string(), eligibility);
    if (result.exact()) {
        ++falseExactCount;
    }
    return expect(!result.exact(), id, "unexpected Exact");
}

bool mutateAndReject(
    const std::filesystem::path& source,
    const std::filesystem::path& target,
    const Probe::Mp4Mp3SampleTableEligibility& eligibility,
    const std::string& boxType,
    std::size_t payloadOffset,
    std::uint32_t replacement,
    const std::string& id,
    int& falseExactCount) {
    std::vector<std::uint8_t> bytes = readBytes(source);
    const std::array<char, 4> type{
        boxType[0], boxType[1], boxType[2], boxType[3]};
    const std::size_t marker = findBoxType(bytes, type);
    if (marker == std::string::npos || marker + payloadOffset + 4 > bytes.size()) {
        return expect(false, id, "mutation target missing");
    }
    writeBe32(bytes, marker + payloadOffset, replacement);
    if (!writeBytes(target, bytes)) {
        return expect(false, id, "could not write mutation");
    }
    const auto result = Probe::probeMp4Mp3SampleEditTablePresentation(
        target.u8string(), eligibility);
    if (result.exact()) {
        ++falseExactCount;
    }
    return expect(!result.exact(), id, "mutation promoted to Exact");
}

int run(
    const std::filesystem::path& fixtureRoot,
    const std::filesystem::path& frozenRoot,
    const std::filesystem::path& golden28) {
    const std::array<ExactCase, 9> generated{{
        {"cbr16", "GEN_CBR_16K.mp4", 0, 160000, 161280, 576},
        {"cbr22", "GEN_CBR_22K.mp4", 0, 220500, 221760, 576},
        {"cbr48", "GEN_CBR_48K.mp4", 0, 480000, 481536, 1152},
        {"classic", "GEN_CLASSIC_10S.mp4", 0, 441000, 442368, 1152},
        {"moov_tail", "GEN_MOOV_TAIL.mp4", 0, 441000, 442368, 1152},
        {"mp3_aac", "GEN_MP3_AAC.mp4", 0, 441000, 442368, 1152},
        {"two_mp3_first", "GEN_TWO_MP3.mp4", 0, 441000, 442368, 1152},
        {"two_mp3_second", "GEN_TWO_MP3.mp4", 1, 529200, 531072, 1152},
        {"video_mp3", "GEN_VIDEO_MP3.mp4", 0, 441000, 442368, 1152},
    }};

    bool ok = validateExactCase(
        golden28, 0, 105840000, 105841152, 1152, "golden28");
    for (const ExactCase& item : generated) {
        ok &= validateExactCase(
            fixtureRoot / item.file,
            item.audioOrdinal,
            item.presentation,
            item.physical,
            item.samplesPerFrame,
            item.name);
    }

    int falseExactCount = 0;
    ok &= validateNonExact(
        fixtureRoot / L"GEN_FRAGMENTED.mp4", 0, "fragmented", falseExactCount);
    ok &= validateNonExact(
        fixtureRoot / L"GEN_VBR_44K.mp4", 0, "vbr", falseExactCount);
    for (const char* prefix : {
             "EA030_", "EA073_", "EA074_", "EA075_",
             "EA076_", "EA077_", "EA078_", "EA079_"}) {
        const std::filesystem::path path = findCase(frozenRoot, prefix);
        ok &= expect(!path.empty(), prefix, "frozen fallback case missing");
        if (!path.empty()) {
            ok &= validateNonExact(path, 0, prefix, falseExactCount);
        }
    }

    const std::filesystem::path fallbackMedia = findCase(frozenRoot, "EA030_");
    ScopedTempDirectory fallbackTemp;
    const std::filesystem::path fallbackProbe = fallbackTemp.path() / L"probe.json";
    if (!fallbackMedia.empty()) {
        const int probeCode = AveMediaBridge_ProbeToJson(
            fallbackMedia.c_str(), fallbackProbe.c_str());
        const std::string probe = readText(fallbackProbe);
        ok &= expect(probeCode == 0, "fallback_product_probe");
        ok &= expect(
            probe.find("\"decodedSampleFramesSource\": \"exact_packet_presentation\"") !=
                std::string::npos,
            "fallback_exact_packet_source",
            "existing exact traversal was not retained");
        ok &= expect(
            probe.find("\"mp4Mp3SampleTableGenericScanEntered\": true") !=
                std::string::npos,
            "fallback_generic_scan",
            "fallback did not enter existing packet scan");
    }

    const std::filesystem::path classic = fixtureRoot / L"GEN_CLASSIC_10S.mp4";
    OpenedMedia classicMedia;
    ok &= expect(openAudioOrdinal(classic, 0, classicMedia), "mutation_base_open");
    Probe::Mp4Mp3SampleTableEligibility mutationEligibility;
    if (classicMedia.selected) {
        mutationEligibility = Probe::evaluateMp4Mp3SampleTableEligibility(
            classic.u8string(), classicMedia.context, classicMedia.selected, false);
    }
    ok &= expect(mutationEligibility.eligible, "mutation_base_eligible");

    ScopedTempDirectory temp;
    const std::vector<std::uint8_t> classicBytes = readBytes(classic);
    const std::size_t stts = findBoxType(classicBytes, {'s', 't', 't', 's'});
    const std::size_t stsz = findBoxType(classicBytes, {'s', 't', 's', 'z'});
    const std::size_t stco = findBoxType(classicBytes, {'s', 't', 'c', 'o'});
    const std::uint32_t sttsCount = readBe32(classicBytes, stts + 12);
    const std::uint32_t stszCount = readBe32(classicBytes, stsz + 12);
    ok &= mutateAndReject(classic, temp.path() / L"stts.mp4", mutationEligibility,
                          "stts", 12, sttsCount + 1, "stts_count", falseExactCount);
    ok &= mutateAndReject(classic, temp.path() / L"stsz.mp4", mutationEligibility,
                          "stsz", 12, stszCount + 1, "stsz_count", falseExactCount);
    ok &= mutateAndReject(classic, temp.path() / L"stsc.mp4", mutationEligibility,
                          "stsc", 12, 2, "stsc_first_chunk", falseExactCount);
    ok &= mutateAndReject(classic, temp.path() / L"stco.mp4", mutationEligibility,
                          "stco", 12, 1, "stco_outside_mdat", falseExactCount);
    ok &= mutateAndReject(classic, temp.path() / L"elst.mp4", mutationEligibility,
                          "elst", 20, 2U << 16, "elst_rate", falseExactCount);

    std::vector<std::uint8_t> damaged = classicBytes;
    const std::uint32_t firstChunkOffset = readBe32(damaged, stco + 12);
    damaged[firstChunkOffset] ^= 0x01U;
    ok &= expect(writeBytes(temp.path() / L"first_header.mp4", damaged),
                 "write_first_header_mutation");
    auto result = Probe::probeMp4Mp3SampleEditTablePresentation(
        (temp.path() / L"first_header.mp4").u8string(), mutationEligibility);
    if (result.exact()) ++falseExactCount;
    ok &= expect(!result.exact(), "first_mp3_header", "corrupt header promoted");

    damaged = classicBytes;
    damaged.insert(damaged.end(), {0x01, 0x02, 0x03, 0x04});
    ok &= expect(writeBytes(temp.path() / L"unknown_tail.mp4", damaged),
                 "write_unknown_tail");
    result = Probe::probeMp4Mp3SampleEditTablePresentation(
        (temp.path() / L"unknown_tail.mp4").u8string(), mutationEligibility);
    if (result.exact()) ++falseExactCount;
    ok &= expect(!result.exact(), "unknown_tail", "unknown tail promoted");

    damaged.assign(classicBytes.begin(), classicBytes.end() - 200);
    ok &= expect(writeBytes(temp.path() / L"truncated.mp4", damaged), "write_truncated");
    result = Probe::probeMp4Mp3SampleEditTablePresentation(
        (temp.path() / L"truncated.mp4").u8string(), mutationEligibility);
    if (result.exact()) ++falseExactCount;
    ok &= expect(!result.exact(), "truncated", "truncation promoted");

    Probe::Mp4Mp3SampleTableProbeOptions options;
    options.hardReadBudgetBytes = 1024;
    result = Probe::probeMp4Mp3SampleEditTablePresentation(
        classic.u8string(), mutationEligibility, options);
    if (result.exact()) ++falseExactCount;
    ok &= expect(!result.exact() && result.maximumBudgetOverrunBytes == 0 &&
                 result.bytesReturned <= options.hardReadBudgetBytes,
                 "budget_exhaustion", "budget contract failed");

    options = {};
    options.forceSeekFailure = true;
    result = Probe::probeMp4Mp3SampleEditTablePresentation(
        classic.u8string(), mutationEligibility, options);
    if (result.exact()) ++falseExactCount;
    ok &= expect(!result.exact(), "seek_failure", "seek failure promoted");

    options = {};
    options.forceReadErrorAfterBytes = 64;
    result = Probe::probeMp4Mp3SampleEditTablePresentation(
        classic.u8string(), mutationEligibility, options);
    if (result.exact()) ++falseExactCount;
    ok &= expect(!result.exact(), "read_failure", "read failure promoted");

    Probe::Mp4Mp3SampleTableEligibility wrongTrack = mutationEligibility;
    wrongTrack.selectedTrackId = 0x7fffffffU;
    result = Probe::probeMp4Mp3SampleEditTablePresentation(
        classic.u8string(), wrongTrack);
    if (result.exact()) ++falseExactCount;
    ok &= expect(!result.exact(), "wrong_track", "wrong track promoted");
    ok &= expect(falseExactCount == 0, "false_exact_count", std::to_string(falseExactCount));

    std::cout << "exactCases=10 fallbackCases=10 robustnessCases=12"
              << " falseExactCount=" << falseExactCount
              << " maximumBudgetOverrunBytes=0\n";
    if (ok) {
        std::cout << "AVEMEDIABRIDGE_MP4_MP3_SAMPLE_EDIT_TABLE_PRESENTATION_AUTHORITY_OK\n";
        return 0;
    }
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cout << "AVEMEDIABRIDGE_MP4_MP3_SAMPLE_EDIT_TABLE_PRESENTATION_AUTHORITY_SKIPPED: "
                     "fixture, frozen, and Golden paths are required\n";
        return 77;
    }
    const std::filesystem::path fixtureRoot(argv[1]);
    const std::filesystem::path frozenRoot(argv[2]);
    const std::filesystem::path golden28(argv[3]);
    if (!std::filesystem::is_directory(fixtureRoot) ||
        !std::filesystem::is_directory(frozenRoot) ||
        !std::filesystem::is_regular_file(golden28)) {
        std::cout << "AVEMEDIABRIDGE_MP4_MP3_SAMPLE_EDIT_TABLE_PRESENTATION_AUTHORITY_SKIPPED: "
                     "required external media unavailable\n";
        return 77;
    }
    return run(fixtureRoot, frozenRoot, golden28);
}
