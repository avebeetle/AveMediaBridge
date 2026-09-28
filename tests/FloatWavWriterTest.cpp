#include "AveMediaBridge/AveMediaBridgeExportApi.h"
#include "Export/FfmpegFloatWavWriter.hpp"
#include "Export/ExportScratchIo.hpp"
#include "ExportOwnedRoot.hpp"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <limits>
#include <vector>
namespace fs = std::filesystem;
static uint32_t u32(const std::vector<uint8_t>& b, size_t p) { return uint32_t(b[p]) | (uint32_t(b[p+1]) << 8) | (uint32_t(b[p+2]) << 16) | (uint32_t(b[p+3]) << 24); }
static uint64_t u64(const std::vector<uint8_t>& b, size_t p) { return uint64_t(u32(b,p)) | (uint64_t(u32(b,p+4)) << 32); }
static uint32_t rateFromFmt(const std::vector<uint8_t>& b) {
    for (size_t p = 12; p + 8 <= b.size();) {
        const auto n = u32(b,p+4);
        if (std::memcmp(b.data()+p, "fmt ", 4) == 0 && n >= 16 && p+8+n <= b.size()) return u32(b,p+12);
        if (p + 8 + n > b.size()) break;
        p += 8 + n + (n & 1);
    }
    return 0;
}
static std::vector<uint8_t> bytes(const fs::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
static std::vector<uint32_t> dataBits(const std::vector<uint8_t>& b) {
    for (size_t p = 12; p + 8 <= b.size();) {
        const auto n = u32(b, p + 4);
        if (std::memcmp(b.data() + p, "data", 4) == 0) {
            if (p + 8 + n > b.size() || n % 4) return {};
            std::vector<uint32_t> result;
            for (size_t i = p + 8; i < p + 8 + n; i += 4) result.push_back(u32(b, i));
            return result;
        }
        p += 8 + n + (n & 1);
    }
    return {};
}
static AMBE_InputV1 input(uint32_t rate, uint32_t channels, uint64_t frames) {
    AMBE_InputV1 x{}; x.structSize = sizeof(x); x.abiVersion = AMBE_ABI_VERSION;
    x.sampleRate = rate; x.channels = channels;
    x.layout = channels == 1 ? AMBE_LAYOUT_MONO : AMBE_LAYOUT_STEREO_LR;
    x.profile = AMBE_PROFILE_WAV_F32_NATIVE_V1; x.expectedFrames = frames; return x;
}
int main() {
    int failures = 0;
    auto expect = [&](bool yes, const char* why) { if (!yes) { std::cerr << "FAIL " << why << '\n'; ++failures; } };
    const fs::path root = acquireOwnedExportTestRoot("ambe-float-wav-");
    const std::vector<uint32_t> monoBits{0x00000000,0x80000000,0x00000001,0x3f800000,0xc0000000};
    for (uint32_t rate : {8000u,44100u,48000u,96000u}) for (uint32_t channels : {1u,2u}) {
        std::vector<uint32_t> bits;
        for (uint32_t b : monoBits) for (uint32_t ch = 0; ch < channels; ++ch) bits.push_back(b);
        std::vector<float> samples(bits.size()); std::memcpy(samples.data(), bits.data(), bits.size() * 4);
        const auto path = root / (std::to_wstring(rate) + L"-" + std::to_wstring(channels) + L".wav");
        { std::ofstream scratch(path, std::ios::binary); }
        const auto spec = input(rate, channels, 5); AMBE_Handle h = 0;
        expect(AveMediaBridge_ExportBegin(path.c_str(), &spec, &h, nullptr, 0) == AMBE_OK && h != 0, "begin valid scratch");
        if (!h) continue;
        expect(AveMediaBridge_ExportWriteF32(h, samples.data(), 4, nullptr, 0) == AMBE_OK, "write first block");
        expect(AveMediaBridge_ExportWriteF32(h, samples.data() + 4 * channels, 1, nullptr, 0) == AMBE_OK, "partial final block");
        AMBE_ResultV1 result{}; result.structSize = sizeof(result); result.abiVersion = AMBE_ABI_VERSION;
        expect(AveMediaBridge_ExportFinish(h, &result, nullptr, 0) == AMBE_OK, "finish");
        expect(result.acceptedFrames == 5 && result.encodedFrames == 5, "exact frames");
        expect(AveMediaBridge_ExportDestroy(h, nullptr, 0) == AMBE_OK, "destroy");
        const auto wav = bytes(path);
        expect(wav.size() >= 44 && (std::memcmp(wav.data(), "RIFF", 4) == 0 || std::memcmp(wav.data(), "RF64", 4) == 0), "WAV container");
        if (wav.size() >= 44) {
            expect(rateFromFmt(wav) == rate, "native sample rate in WAV fmt chunk");
            expect(dataBits(wav) == bits, "finite PCM bits unchanged");
        }
        if (rate == 48000) {
            wchar_t evidence[MAX_PATH]{};
            if (GetEnvironmentVariableW(L"AMBE_EXPORT_EVIDENCE_ROOT", evidence, MAX_PATH) > 0)
                fs::copy_file(path, fs::path(evidence) /
                    (channels == 1 ? L"finite-mono-48000.wav" : L"finite-stereo-48000.wav"),
                    fs::copy_options::overwrite_existing);
        }
        fs::remove(path);
    }
    for (uint32_t bits : {0x7fc00000u,0x7f800000u,0xff800000u}) {
        const auto path = root / (std::to_wstring(bits) + L"-bad.wav");
        { std::ofstream scratch(path, std::ios::binary); }
        const auto spec = input(48000, 1, 1); AMBE_Handle h = 0;
        expect(AveMediaBridge_ExportBegin(path.c_str(), &spec, &h, nullptr, 0) == AMBE_OK, "begin nonfinite case");
        if (h) {
            float sample; std::memcpy(&sample, &bits, 4);
            expect(AveMediaBridge_ExportWriteF32(h, &sample, 1, nullptr, 0) == AMBE_INVALID_ARGUMENT, "nonfinite rejected");
            AveMediaBridge_ExportDestroy(h, nullptr, 0);
        }
        fs::remove(path);
    }
    {
        const auto path = root / L"rf64.wav"; { std::ofstream scratch(path, std::ios::binary); }
        const auto spec = input(48000, 1, 5);
        auto writer = AveMediaBridge::Export::makeFloatWavWriter(path, spec, true);
        std::vector<float> samples(monoBits.size()); std::memcpy(samples.data(), monoBits.data(), 20);
        writer->write(samples.data(), 4); writer->write(samples.data() + 4, 1);
        expect(writer->finish() == 5, "forced RF64 exact frame count"); writer.reset();
        const auto wav = bytes(path);
        expect(wav.size() >= 80 && std::memcmp(wav.data(), "RF64", 4) == 0 &&
            std::memcmp(wav.data() + 12, "ds64", 4) == 0, "forced RF64 has ds64 header");
        if (wav.size() >= 80) {
            expect(u64(wav, 28) == 20 && u64(wav, 36) == 5, "RF64 ds64 has exact data bytes and frames");
            std::vector<uint32_t> lastBits;
            for (size_t p = wav.size() - 20; p < wav.size(); p += 4) lastBits.push_back(u32(wav,p));
            expect(lastBits == monoBits, "RF64 data bits unchanged");
        }
        wchar_t evidence[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"AMBE_EXPORT_EVIDENCE_ROOT", evidence, MAX_PATH) > 0)
            fs::copy_file(path, fs::path(evidence) / L"forced-rf64-mono-48000.wav",
                fs::copy_options::overwrite_existing);
        fs::remove(path);
    }
    for (auto fault : {AveMediaBridge::Export::ExportScratchIo::Fault::DiskFull,
                       AveMediaBridge::Export::ExportScratchIo::Fault::ShortWrite}) {
        const auto path = root / (std::to_wstring(static_cast<int>(fault)) + L"-job.scratch");
        const auto target = root / (std::to_wstring(static_cast<int>(fault)) + L"-must-not-publish.wav");
        { std::ofstream scratch(path, std::ios::binary); }
        const auto spec = input(48000, 2, 131072);
        std::vector<float> samples(262144, 0.25f);
        {
            auto writer = AveMediaBridge::Export::makeFloatWavWriter(path, spec, false, fault);
            AveMediaBridge::Export::StreamingExportJob job(spec, std::move(writer));
            const auto status = job.write(samples.data(), 131072);
            AMBE_ResultV1 result{}; result.structSize=sizeof(result); result.abiVersion=AMBE_ABI_VERSION;
            expect(status != AMBE_OK && job.state() == AveMediaBridge::Export::StreamingExportJob::State::Failed,
                "actual scratch write fault reaches failed export job");
            expect(job.finish(&result) != AMBE_OK && result.encodedFrames == 0,
                "failed real writer cannot return successful finalized frames");
            expect(job.abort() == AMBE_OK && fs::remove(path), "abort releases actual scratch for owner cleanup");
            expect(!fs::exists(target), "failed scratch writer never publishes target");
        }
    }
    fs::remove_all(root); return failures ? 1 : 0;
}
