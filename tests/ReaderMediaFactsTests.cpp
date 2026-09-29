#include "../src/Probe/ReaderMediaFacts.hpp"
#include "AveMediaBridge/AveMediaBridgeMediaFactsApi.h"
#include <stdexcept>
#include <iostream>
#include <type_traits>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include <cstdlib>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace fs = std::filesystem;

using AveMediaBridge::Probe::captureReaderMediaFacts;
static_assert(std::is_same_v<decltype(&AveMediaBridge_ReaderGetMediaFactsV1),
    int(__cdecl*)(AMBR_PreparedInput*, char*, uint32_t, uint32_t*)>);

static void check(bool ok, const char* name) { if (!ok) throw std::runtime_error(name); }
static AVStream* stream(AVFormatContext* context, AVMediaType type, AVCodecID codec,
    int disposition, int id, int64_t duration) {
    auto* s = avformat_new_stream(context, nullptr);
    check(s != nullptr, "stream allocation");
    s->codecpar->codec_type = type;
    s->codecpar->codec_id = codec;
    s->codecpar->width = 16;
    s->codecpar->height = 16;
    s->disposition = disposition;
    s->id = id;
    s->duration = duration;
    s->time_base = {1, 1000};
    s->avg_frame_rate = {1, 1};
    return s;
}
struct InputBytes {
    std::vector<char> data;
    bool failReads = false;
};
static AMBI_Status __cdecl readAt(void* user, std::uint64_t offset, void* destination,
    std::uint32_t requested, std::uint32_t* count) {
    auto& input = *static_cast<InputBytes*>(user);
    *count = 0;
    if (input.failReads || offset > input.data.size() || requested > input.data.size() - offset)
        return AMBI_IO_ERROR;
    std::memcpy(destination, input.data.data() + offset, requested);
    *count = requested;
    return AMBI_OK;
}
static AMBI_Status __cdecl checkCancel(void*) { return AMBI_OK; }
static void realCase(const fs::path& fixture, const fs::path& root, const char* classification,
    unsigned count, bool fault) {
    std::ifstream file(fixture, std::ios::binary);
    check(file.is_open(), "fixture open");
    InputBytes input{{std::istreambuf_iterator<char>(file), {}}, false};
    check(!input.data.empty(), "fixture bytes");
    AMBI_SourceV1 source{};
    source.structSize = sizeof(source); source.abiVersion = AMBI_ABI_VERSION;
    source.byteSize = input.data.size(); source.sourceToken[0] = 0x42;
    source.user = &input; source.readAt = readAt; source.checkCancel = checkCancel;
    AMBR_PrepareOptionsV1 options{};
    options.structSize = sizeof(options); options.abiVersion = AMBR_ABI_VERSION;
    options.source = &source; options.displayLabel = L"Z:\\missing\\display-only.mp4";
    AMBR_PreparedInput* handle = nullptr;
    check(AveMediaBridge_ReaderPrepareV1(&options, &handle) == AMBR_OK && handle,
        "real prepared input");
    struct Guard { AMBR_PreparedInput* p; ~Guard() { AveMediaBridge_ReaderDestroyV1(p); } } guard{handle};
    std::uint32_t required = 0;
    check(AveMediaBridge_ReaderGetMediaFactsV1(handle, nullptr, 0, &required) ==
        AMBM_BUFFER_TOO_SMALL && required > 1 && required <= AMBM_MAX_JSON_BYTES,
        "facts size query");
    std::array<char, 3> tiny{'x', 'x', 'x'};
    std::uint32_t again = 0;
    check(AveMediaBridge_ReaderGetMediaFactsV1(handle, tiny.data(), tiny.size(), &again) ==
        AMBM_BUFFER_TOO_SMALL && again == required && tiny[0] == 'x' && tiny[2] == 'x',
        "no partial facts JSON");
    std::string json(required, '\0');
    check(AveMediaBridge_ReaderGetMediaFactsV1(handle, json.data(), required, &again) ==
        AMBR_OK && again == required && json.back() == '\0', "facts fetch");
    json.pop_back();
    check(json.find("Z:") == std::string::npos &&
        json.find(std::string("\"classification\":\"") + classification + "\"") != std::string::npos &&
        json.find("\"timedVideoCount\":" + std::to_string(count)) != std::string::npos,
        "source-bound video facts");
    if (count != 1) check(json.find("\"video\":{\"classification\":\"" +
        std::string(classification) + "\",\"timedVideoCount\":" + std::to_string(count) +
        ",\"reason\":\"") != std::string::npos &&
        json.find("\"streamIndex\":1,\"trackId\":") == std::string::npos,
        "no arbitrary video binding");
    if (fault) input.failReads = true;
    const auto out = root / fixture.stem();
    check(fs::create_directory(out), "fresh import directory");
    AMBR_ImportOptionsV1 import{};
    import.structSize = sizeof(import); import.abiVersion = AMBR_ABI_VERSION;
    import.sessionMediaDir = out.c_str();
    const int importResult = AveMediaBridge_ReaderImportV1(handle, &import);
    if (fault) check(importResult != AMBR_OK, "BadSourceIsNotOptionalVideo");
    else check(importResult == AMBR_OK, "FactsDoNotConsumeImport");
}
int main(int argc, char** argv) {
    try {
        AveMediaBridge::Input::SelectedAudioBinding binding;
        binding.sourceToken[0] = 0x42;
        binding.byteSize = 1234;
        binding.demuxerName = "mov,mp4,m4a,3gp,3g2,mj2";
        binding.streamIndex = 0; binding.trackId = 1;
        binding.codecId = AV_CODEC_ID_AAC; binding.sampleRate = 48000;
        binding.channels = 2; binding.timeBaseNumerator = 1;
        binding.timeBaseDenominator = 48000;
        auto* ctx = avformat_alloc_context();
        check(ctx != nullptr, "context allocation");
        stream(ctx, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC, 0, 1, 1000);
        stream(ctx, AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_MJPEG, AV_DISPOSITION_ATTACHED_PIC, 2, 1000);
        auto cover = captureReaderMediaFacts(ctx, binding);
        check(cover.videoClassification == "none" && cover.timedVideoCount == 0, "CoverIsNotTimedVideo");
        check(encodeReaderMediaFacts(cover).find("\"selectedAudio\"") != std::string::npos,
            "selected audio encoded");
        stream(ctx, AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264, 0, 3, 1000);
        auto one = captureReaderMediaFacts(ctx, binding);
        check(one.videoClassification == "singleTimed" && one.timedVideoCount == 1 &&
            one.videoStreamIndex == 2 && one.videoTrackId == 3, "OneTimedVideoBinding");
        stream(ctx, AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264, 0, 4, 1000);
        auto two = captureReaderMediaFacts(ctx, binding);
        check(two.videoClassification == "multipleTimed" && two.timedVideoCount == 2 &&
            two.videoStreamIndex == -1, "TwoTimedVideosNoChoice");
        ctx->streams[3]->duration = AV_NOPTS_VALUE;
        ctx->streams[3]->avg_frame_rate = {0, 1};
        ctx->streams[3]->r_frame_rate = {0, 1};
        auto uncertain = captureReaderMediaFacts(ctx, binding);
        check(uncertain.videoClassification == "unknown", "UncertainVideoTiming");
        ctx->streams[3]->duration = 1000;
        ctx->streams[3]->avg_frame_rate = {1, 1};
        ctx->streams[2]->codecpar->codec_id = AV_CODEC_ID_NONE;
        auto unsupported = captureReaderMediaFacts(ctx, binding);
        check(unsupported.videoClassification == "unknown", "UnsupportedVideoCodec");
        avformat_free_context(ctx);
        std::uint32_t required = 0;
        check(AveMediaBridge_ReaderGetMediaFactsV1(nullptr, nullptr, 0, &required) == AMBR_INVALID_ARGUMENT,
            "null handle refused");
        if (argc == 2) {
            const fs::path fixtures(argv[1]);
            const char* rootText = std::getenv("AVEVOICE_DATA_ROOT");
            check(rootText && fs::is_directory(rootText), "isolated data root");
            const auto root = fs::path(rootText) /
                ("real-media-facts-" + std::to_string(GetCurrentProcessId()) + "-" +
                    std::to_string(GetTickCount64()));
            check(fs::create_directory(root), "fresh real facts root");
            realCase(fixtures / "reader_cover_only_aac.mp4", root, "none", 0, false);
            realCase(fixtures / "reader_single_video_aac.mp4", root, "singleTimed", 1, false);
            realCase(fixtures / "reader_two_video_aac.mp4", root, "multipleTimed", 2, false);
            check(fs::create_directory(root / "fault"), "fresh fault directory");
            realCase(fixtures / "reader_single_video_aac.mp4", root / "fault", "singleTimed", 1, true);
        }
        std::cout << "reader media facts checks passed\n";
        return 0;
    } catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}
