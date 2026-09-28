#include "AveMediaBridge/AveMediaBridgeApi.hpp"
#include "AveMediaBridge/AveMediaBridgeReaderApi.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace fs = std::filesystem;
struct Checks {
    int n = 0, failed = 0;
    void expect(bool ok, const std::string &label) {
        ++n;
        if (!ok) {
            ++failed;
            std::cerr << "FAIL " << label << '\n';
        }
    }
};
std::vector<char> load(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
struct Bytes {
    std::vector<char> data;
    int reads = 0, checks = 0, failRead = -1, cancelCheck = -1;
    bool cancel = false, throws = false, checkThrows = false;
    int opens = 0, cancelOpen = -1, failOpen = -1;
};
AMBI_Status __cdecl readAt(void *u, uint64_t offset, void *dest, uint32_t size, uint32_t *got) {
    auto &b = *static_cast<Bytes *>(u);
    *got = 0;
    ++b.reads;
    if (offset == 0)
        ++b.opens;
    if (b.cancelOpen >= 0 && b.opens >= b.cancelOpen)
        return AMBI_CANCELED;
    if (b.failOpen >= 0 && b.opens >= b.failOpen)
        return AMBI_IO_ERROR;
    if (b.throws)
        throw std::runtime_error("injected read exception");
    if (b.failRead >= 0 && b.reads >= b.failRead)
        return AMBI_IO_ERROR;
    if (offset > b.data.size() || size > b.data.size() - offset)
        return AMBI_IO_ERROR;
    std::memcpy(dest, b.data.data() + offset, size);
    *got = size;
    return AMBI_OK;
}
AMBI_Status __cdecl canceled(void *u) {
    auto &b = *static_cast<Bytes *>(u);
    ++b.checks;
    if (b.checkThrows)
        throw std::runtime_error("cancel callback exception");
    return b.cancel || (b.cancelCheck >= 0 && b.checks >= b.cancelCheck) ? AMBI_CANCELED : AMBI_OK;
}
AMBI_SourceV1 descriptor(Bytes &b) {
    AMBI_SourceV1 s{};
    s.structSize = sizeof s;
    s.abiVersion = 1;
    s.byteSize = b.data.size();
    s.sourceToken[0] = 0x42;
    s.sourceToken[15] = 0xEF;
    s.user = &b;
    s.readAt = readAt;
    s.checkCancel = canceled;
    return s;
}
int prepare(Bytes &b, AMBR_PreparedInput **h,
            const wchar_t *label = L"Z:/does-not-exist/label.mp4") {
    auto s = descriptor(b);
    AMBR_PrepareOptionsV1 o{};
    o.structSize = sizeof o;
    o.abiVersion = 1;
    o.source = &s;
    o.displayLabel = label;
    return AveMediaBridge_ReaderPrepareV1(&o, h);
}
struct Handle {
    AMBR_PreparedInput *p = nullptr;
    ~Handle() { AveMediaBridge_ReaderDestroyV1(p); }
};
struct Progress {
    fs::path dir;
    Bytes *bytes = nullptr;
    bool valid = true;
    uint64_t frames = 0;
    int calls = 0;
    int action = 0;
    bool stop = false;
    std::vector<float> waveform;
    std::vector<double> squares, abs;
    std::vector<uint64_t> counts;
};
void __stdcall progress(const AveMediaBridgeImportProgress *p, void *u) {
    auto &s = *static_cast<Progress *>(u);
    ++s.calls;
    auto bytes = load(s.dir / "original_f32.bin");
    s.valid = s.valid && p->framesWritten >= s.frames && bytes.size() >= p->bytesWritten &&
              p->bytesWritten == p->framesWritten * p->channels * 4;
    s.frames = p->framesWritten;
    if (s.action == 1 && p->framesWritten > 0)
        s.stop = true;
    if (s.action == 2 && p->progress01 == 1)
        s.stop = true;
    if (s.action == 3 && p->framesWritten > 0)
        throw std::runtime_error("progress exception");
    if (s.action == 4 && p->progress01 == 1)
        s.bytes->cancel = true;
    if (s.action == 5 && p->framesWritten > 0)
        s.bytes->failRead = s.bytes->reads + 1;
    if (s.action == 6 && p->progress01 == 1)
        s.bytes->checkThrows = true;
}
int __stdcall cancelImport(void *u) {
    const auto& s = *static_cast<Progress *>(u);
    return s.stop || (s.action == 7 && fs::exists(s.dir / "original_f32.bin")) ||
        (s.action == 9 && s.bytes->opens >= 2) ? 1 : 0;
}
void __stdcall wave(const AveMediaBridgeWaveformChunk *p, void *u) {
    auto &s = *static_cast<Progress *>(u);
    if (s.action == 8) throw std::runtime_error("waveform callback exception");
    s.waveform.insert(s.waveform.end(), p->minMaxPairs,
                      p->minMaxPairs + p->binCount * p->valuesPerBin);
    if (p->sumSquaresPerBin)
        s.squares.insert(s.squares.end(), p->sumSquaresPerBin, p->sumSquaresPerBin + p->binCount);
    if (p->sumAbsPerBin)
        s.abs.insert(s.abs.end(), p->sumAbsPerBin, p->sumAbsPerBin + p->binCount);
    if (p->frameCountPerBin)
        s.counts.insert(s.counts.end(), p->frameCountPerBin, p->frameCountPerBin + p->binCount);
}
AMBR_ImportOptionsV1 options(const std::wstring &dir, Progress *p = nullptr) {
    AMBR_ImportOptionsV1 o{};
    o.structSize = sizeof o;
    o.abiVersion = 1;
    o.sessionMediaDir = dir.c_str();
    if (p) {
        o.onProgress = progress;
        o.shouldCancel = cancelImport;
        o.onWaveformChunk = wave;
        o.userData = p;
    }
    return o;
}
bool noArtifacts(const fs::path &dir) {
    for (auto name : {"original_f32.bin", "audio_info.json", "metadata.json", "audio_info.json.tmp",
                      "metadata.json.tmp"})
        if (fs::exists(dir / name))
            return false;
    return true;
}
// Only explicit provenance and observed scan wall-clock telemetry may differ.
std::string normalizedJson(const fs::path &p, bool cross = false) {
    auto bytes = load(p);
    std::istringstream in(std::string(bytes.begin(), bytes.end()));
    std::string line, out;
    for (; std::getline(in, line);) {
        bool exclude = false;
        for (auto key : {"sourcePath", "mp4Mp3SampleTableScanDurationUs",
                         "matroskaAacSequentialScanDurationUs"})
            if (line.find(std::string("\"") + key + "\":") != std::string::npos)
                exclude = true;
        if (cross && (line.find("\"inputAuthority\":") != std::string::npos ||
                      line.find("\"probeScore\":") != std::string::npos))
            exclude = true;
        if (!exclude)
            out += line + '\n';
    }
    return out;
}
struct Row {
    const char *name;
    fs::path input;
    bool supported;
    int result;
};
int main(int argc, char **argv) {
    Checks c;
    if (argc != 3)
        return 2;
    const char *env = std::getenv("AVEVOICE_DATA_ROOT");
    if (!env || !fs::is_directory(env))
        return 2;
    fs::path fixtures = argv[1], lab = argv[2];
    auto root =
        fs::path(env) /
        ("task3-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    auto frozen = fixtures.parent_path() / "frozen-legacy";
    std::vector<Row> rows = {
        {"EA035-v2", lab / "exact_authority/valid/aac/EA035_m4a_aac_96000_10241.m4a", true, 0},
        {"EA127-v2", lab / "exact_authority/valid/aac/EA127_mp4_aac_88200_88199.mp4", true, 0},
        {"EA098-v3", lab / "exact_authority/valid/aac/EA098_m4a_aac_48000_1.m4a", true, 2},
        {"AV018-v2", lab / "av_sync/valid/mp4/AV018_nonzero_audio_media_time_mp4.mp4", true, 0},
        {"AV019-v2", lab / "av_sync/valid/mov/AV019_empty_edit_before_audio_mov.mov", true, 0},
        {"reader_demux_aac-v2", fixtures / "reader_demux_aac.m4a", true, 0},
        {"reader_front-v1", fixtures / "reader_stereo_front_aac.mp4", true, 0},
        {"reader_tail-v1", fixtures / "reader_stereo_tail_aac.mp4", true, 0},
        {"reader_two_aac-v1", fixtures / "reader_two_aac_default_second.mp4", true, 0},
        {"reader_mp4_mp3-v1", fixtures / "reader_mp4_mp3_control.mp4", false, 0},
        {"reader_mp4_alac-v1", fixtures / "reader_mp4_alac_control.m4a", false, 0},
        {"reader_mp4_no_audio-v1", fixtures / "reader_mp4_no_audio.mp4", false, 2},
        {"EA002-wav-v1", lab / "exact_authority/valid/pcm_lossless/EA002_wav_s16_16000_257.wav",
         false, 0},
        {"EA029-mp3-v1", lab / "exact_authority/valid/mp3/EA029_mp3_44100_11521.mp3", false, 0},
        {"EA104-flac-v1", lab / "exact_authority/valid/pcm_lossless/EA104_flac_8000_1023.flac",
         false, 0},
        {"EA038-mka-aac-v1", lab / "exact_authority/valid/aac/EA038_mkv_aac_44100_10241.mka", false,
         0}};
    for (const auto &row : rows) {
        const std::string label = row.name;
        auto dir = root / row.name;
        fs::create_directory(dir);
        auto legacy = dir / "legacy";
        fs::create_directory(legacy);
        auto reader = dir / "reader";
        fs::create_directory(reader);
        c.expect(fs::is_regular_file(row.input) &&
                     fs::is_regular_file(frozen / row.name / "probe.json"),
                 label + " fixtures and frozen oracle present");
        auto frozenInput = frozen / row.name / "input" / row.input.filename();
        c.expect(fs::is_regular_file(frozenInput) && load(frozenInput) == load(row.input),
                 label + " frozen source bytes");
        auto input = frozenInput.wstring(), ld = legacy.wstring(), rd = reader.wstring();
        auto pp = dir / "probe.json";
        c.expect(AveMediaBridge_ProbeToJson(input.c_str(), pp.c_str()) == 0,
                 label + " legacy probe");
        c.expect(normalizedJson(pp) == normalizedJson(frozen / row.name / "probe.json"),
                 label + " frozen probe");
        Progress lp{legacy}, rp{reader};
        AveMediaBridgeImportOptions lo{};
        lo.structSize = sizeof lo;
        lo.inputPath = input.c_str();
        lo.sessionMediaDir = ld.c_str();
        lo.onProgress = progress;
        lo.shouldCancel = cancelImport;
        lo.onWaveformChunk = wave;
        lo.userData = &lp;
        c.expect(AveMediaBridge_ImportAudioToSessionEx(&lo) == row.result,
                 label + " frozen terminal result");
        for (auto name : {"metadata.json", "audio_info.json", "original_f32.bin"})
            if (row.result == 0) {
                c.expect(fs::exists(legacy / name) &&
                             fs::exists(frozen / row.name / "session" / name),
                         label + " artifact exists " + name);
                c.expect(std::string(name) == "original_f32.bin"
                             ? load(legacy / name) == load(frozen / row.name / "session" / name)
                             : normalizedJson(legacy / name) ==
                                   normalizedJson(frozen / row.name / "session" / name),
                         label + " frozen full artifact " + name);
            }
        Bytes bytes{load(row.input)};
        Handle h;
        int prepared = prepare(bytes, &h.p);
        c.expect(prepared == (row.supported ? 0 : AMBR_UNSUPPORTED), label + " prepare result");
        if (!h.p)
            continue;
        auto probe = reader / "reader.json";
        c.expect(AveMediaBridge_ReaderWriteProbeJsonV1(h.p, probe.c_str()) == 0,
                 label + " reader probe");
        c.expect(normalizedJson(probe, true) == normalizedJson(pp, true),
                 label + " cross route probe");
        auto readerJson = load(probe);
        auto legacyJson = load(pp);
        std::string rj(readerJson.begin(), readerJson.end()),
            lj(legacyJson.begin(), legacyJson.end());
        c.expect(rj.find("\"probeScore\": 0,") != std::string::npos &&
                     lj.find("\"probeScore\": 0,") == std::string::npos,
                 label + " honest forced MOV score");
        c.expect(
            rj.find("\"sourceToken\": \"420000000000000000000000000000ef\"") != std::string::npos &&
                rj.find("\"byteSize\": " + std::to_string(bytes.data.size())) != std::string::npos,
            label + " captured authority");
        c.expect(rj.find("\"selectedAudioStreamIndex\": " +
                         std::to_string(label == "reader_two_aac-v1" ? 1 : 0) + "},") !=
                     std::string::npos,
                 label + " captured selected stream index");
        {
            std::ofstream poison(reader / "probe.json");
            poison << "{\"decodedSampleFrames\":1,\"decodedSampleFramesTrust\":\"authoritative\"}";
        }
        auto o = options(rd, &rp);
        c.expect(AveMediaBridge_ReaderImportV1(h.p, &o) == row.result,
                 label + " reader terminal result");
        c.expect(AveMediaBridge_ReaderImportV1(h.p, &o) == AMBR_WRONG_STATE,
                 label + " one attempt");
        c.expect(lp.valid && rp.valid, label + " progress committed bytes readable");
        c.expect(lp.waveform == rp.waveform && lp.squares == rp.squares && lp.abs == rp.abs &&
                     lp.counts == rp.counts,
                 label + " all waveform values");
        if (row.result == 0)
            for (auto name : {"metadata.json", "audio_info.json", "original_f32.bin"})
                c.expect(std::string(name) == "original_f32.bin"
                             ? load(legacy / name) == load(reader / name)
                             : normalizedJson(legacy / name) == normalizedJson(reader / name),
                         label + " cross route full artifact " + name);
        else
            c.expect(noArtifacts(reader), label + " failure cleanup");
        int reads = bytes.reads, checks = bytes.checks, calls = rp.calls;
        AveMediaBridge_ReaderDestroyV1(h.p);
        h.p = nullptr;
        c.expect(bytes.reads == reads && bytes.checks == checks && rp.calls == calls,
                 label + " no callback after return/destroy");
    }
    for (int action = 0; action <= 8; ++action) {
        Bytes b{load(fixtures / "reader_demux_aac.m4a")};
        Handle h;
        c.expect(prepare(b, &h.p) == 0, "failure prepare");
        if (!h.p)
            continue;
        auto dir = root / ("fault-" + std::to_string(action));
        fs::create_directory(dir);
        auto ds = dir.wstring();
        Progress p{dir};
        p.bytes = &b;
        p.action = action;
        auto o = options(ds, &p);
        if (action == 0)
            p.stop = true;
        int result = AveMediaBridge_ReaderImportV1(h.p, &o);
        c.expect(result == ((action == 3 || action == 5 || action == 6 || action == 8) ? AMBR_INPUT_FAILED
                                                                        : AMBR_CANCELED),
                 "phase failure " + std::to_string(action));
        c.expect(noArtifacts(dir), "phase cleanup " + std::to_string(action));
        if (action == 7) c.expect(p.frames == 0 && p.calls == 0, "early decode cancellation before committed prefix");
        c.expect(AveMediaBridge_ReaderImportV1(h.p, &o) == AMBR_WRONG_STATE,
                 "failed attempt terminal");
    }
    for (int mode = 0; mode < 3; ++mode) {
        Bytes b{load(fixtures / "reader_demux_aac.m4a")};
        b.cancel = mode == 0;
        b.failRead = mode == 1 ? 1 : -1;
        b.throws = mode == 2;
        Handle h;
        c.expect(prepare(b, &h.p) == (mode == 0 ? AMBR_CANCELED : AMBR_INPUT_FAILED),
                 "prepare fault");
        c.expect(h.p == nullptr, "no failed handle");
    }
    for (int mode = 0; mode < 5; ++mode) {
        Bytes b{load(fixtures / "reader_demux_aac.m4a")};
        Handle h;
        c.expect(prepare(b, &h.p) == 0, "scan failure prepare");
        if (!h.p)
            continue;
        b.opens = 0;
        if (mode == 0)
            b.cancelOpen = 2;
        if (mode == 1)
            b.failOpen = 2;
        if (mode == 2)
            b.failRead = b.reads + 1;
        if (mode == 3)
            b.throws = true;
        auto dir = root / ("scan-fault-" + std::to_string(mode));
        fs::create_directory(dir);
        auto ds = dir.wstring();
        Progress p{dir}; p.bytes = &b; p.action = mode == 4 ? 9 : 0;
        auto o = options(ds, &p);
        c.expect(AveMediaBridge_ReaderImportV1(h.p, &o) ==
                     (mode == 0 || mode == 4 ? AMBR_CANCELED : AMBR_INPUT_FAILED),
                 "discovery/scan failure result");
        c.expect(noArtifacts(dir), "discovery/scan failure cleanup");
        if (mode < 2 || mode == 4)
            c.expect(b.opens == 2, "failure reached prescan second demux");
    }
    {
        Bytes b{load(fixtures / "reader_stereo_front_aac.mp4")};
        Handle h;
        auto owned = root / "removed-label.mp4";
        fs::copy_file(fixtures / "reader_stereo_front_aac.mp4", owned);
        c.expect(prepare(b, &h.p, owned.c_str()) == 0, "owned label prepare");
        fs::remove(owned);
        if (h.p) {
            auto dir = root / "validation";
            fs::create_directory(dir);
            auto ds = dir.wstring();
            auto o = options(ds);
            c.expect(AveMediaBridge_ReaderWriteProbeJsonV1(h.p, root.c_str()) == AMBR_OUTPUT_FAILED,
                     "probe output failure nonterminal");
            for (int mode = 0; mode < 4; ++mode) {
                auto invalid = o;
                if (mode == 0)
                    invalid.structSize--;
                if (mode == 1)
                    invalid.abiVersion++;
                if (mode == 2)
                    invalid.reserved[2] = 1;
                if (mode == 3)
                    invalid.sessionMediaDir = L"relative";
                c.expect(AveMediaBridge_ReaderImportV1(h.p, &invalid) == AMBR_INVALID_ARGUMENT,
                         "options validation preserves attempt");
            }
            for (auto name : {"original_f32.bin", "audio_info.json", "metadata.json",
                              "audio_info.json.tmp", "metadata.json.tmp"}) {
                {
                    std::ofstream sentinel(dir / name);
                    sentinel << "existing";
                }
                c.expect(AveMediaBridge_ReaderImportV1(h.p, &o) == AMBR_INVALID_ARGUMENT,
                         "artifact collision refused");
                c.expect(load(dir / name) ==
                             std::vector<char>({'e', 'x', 'i', 's', 't', 'i', 'n', 'g'}),
                         "pre-existing artifact retained");
                fs::remove(dir / name);
            }
            c.expect(AveMediaBridge_ReaderImportV1(h.p, &o) == 0,
                     "deleted display label cannot affect source");
            c.expect(load(dir / "original_f32.bin") ==
                         load(frozen / "reader_front-v1/session/original_f32.bin"),
                     "deleted-label PCM bytes");
            c.expect(
                AveMediaBridge_ReaderWriteProbeJsonV1(h.p, (dir / "diagnostic.json").c_str()) == 0,
                "terminal diagnostic probe available");
        }
    }
    {
        Bytes b{load(fixtures / "reader_stereo_front_aac.mp4")};
        Handle h;
        c.expect(prepare(b, &h.p) == 0, "binding mutation prepare");
        const char type[] = "tkhd";
        auto at = std::search(b.data.begin(), b.data.end(), type, type + 4);
        c.expect(at != b.data.end(), "owned track header located");
        if (h.p && at != b.data.end()) {
            const auto index = static_cast<size_t>(at - b.data.begin());
            b.data.at(index + 19) ^= 0x10;
            auto dir = root / "binding-mismatch";
            fs::create_directory(dir);
            auto ds = dir.wstring();
            auto o = options(ds);
            c.expect(AveMediaBridge_ReaderImportV1(h.p, &o) == AMBR_BINDING_MISMATCH,
                     "changed track identity rejected");
            c.expect(noArtifacts(dir), "binding failure before PCM");
        }
    }
    for (size_t length : {size_t(32767), size_t(32768)}) {
        Bytes b{load(fixtures / "reader_stereo_front_aac.mp4")};
        Handle h;
        std::wstring label(length, L'x');
        c.expect(prepare(b, &h.p, label.c_str()) == (length == 32767 ? 0 : AMBR_INVALID_ARGUMENT),
                 "label length bound");
    }
    {
        Bytes malformed{std::vector<char>(64, 0)};
        Handle h;
        c.expect(prepare(malformed, &h.p) == AMBR_UNSUPPORTED && h.p == nullptr,
                 "malformed in-memory source rejected");
        Bytes truncated{load(fixtures / "reader_stereo_tail_aac.mp4")};
        truncated.data.resize(128);
        c.expect(prepare(truncated, &h.p) == AMBR_UNSUPPORTED && h.p == nullptr,
                 "truncated tail moov rejected");
        Bytes changed{load(fixtures / "reader_stereo_front_aac.mp4")};
        c.expect(prepare(changed, &h.p) == 0, "truncation after prepare");
        if (h.p) {
            changed.data.resize(128);
            auto dir = root / "truncated-after-prepare";
            fs::create_directory(dir);
            auto ds = dir.wstring();
            auto o = options(ds);
            c.expect(AveMediaBridge_ReaderImportV1(h.p, &o) == AMBR_INPUT_FAILED,
                     "source no longer satisfies descriptor fails");
            c.expect(noArtifacts(dir), "truncated import cleanup");
        }
    }
    {
        auto run = [&](int index) {
            Bytes b{load(fixtures / "reader_stereo_front_aac.mp4")};
            Handle h;
            if (prepare(b, &h.p, index == 1 ? nullptr : L"missing-label.mp4") != 0)
                return false;
            auto dir = root / ("concurrent-" + std::to_string(index));
            fs::create_directory(dir);
            auto ds = dir.wstring();
            auto o = options(ds);
            return AveMediaBridge_ReaderImportV1(h.p, &o) == 0 &&
                   load(dir / "original_f32.bin") ==
                       load(frozen / "reader_front-v1/session/original_f32.bin");
        };
        auto first = std::async(std::launch::async, run, 1),
             second = std::async(std::launch::async, run, 2);
        c.expect(first.get() && second.get(), "two independent handles concurrently");
    }
    std::cout << "Evidence " << root << "\n" << c.n << " checks, " << c.failed << " failures\n";
    return c.failed ? 1 : 0;
}
