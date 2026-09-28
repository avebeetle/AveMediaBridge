#include "AveMediaBridge/AveMediaBridgeExportApi.h"
#include "Export/ExportScratchIo.hpp"
#include "ExportOwnedRoot.hpp"
extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
}
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <windows.h>
namespace fs = std::filesystem;
int main() {
    int failures = 0; auto expect = [&](bool yes, const char* why) { if (!yes) { std::cerr << "FAIL " << why << '\n'; ++failures; } };
    const fs::path root = acquireOwnedExportTestRoot("ambe-scratch-");
    AMBE_InputV1 spec{}; spec.structSize = sizeof(spec); spec.abiVersion = AMBE_ABI_VERSION;
    spec.sampleRate = 48000; spec.channels = 1; spec.layout = AMBE_LAYOUT_MONO;
    spec.profile = AMBE_PROFILE_WAV_F32_NATIVE_V1; spec.expectedFrames = 1;
    AMBE_Handle h = 9; const auto missing = root / L"missing.wav";
    expect(AveMediaBridge_ExportBegin(missing.c_str(), &spec, &h, nullptr, 0) != AMBE_OK && h == 0 && !fs::exists(missing), "missing scratch never created");
    const auto parent = root / L"missing-parent" / L"scratch.wav"; h = 9;
    expect(AveMediaBridge_ExportBegin(parent.c_str(), &spec, &h, nullptr, 0) != AMBE_OK && h == 0 && !fs::exists(parent.parent_path()), "missing parent never created");
    const auto nonempty = root / L"nonempty.wav"; { std::ofstream f(nonempty, std::ios::binary); f << 'x'; } h = 9;
    expect(AveMediaBridge_ExportBegin(nonempty.c_str(), &spec, &h, nullptr, 0) != AMBE_OK && h == 0 && fs::file_size(nonempty) == 1, "nonempty scratch untouched");
    const auto empty = root / L"empty.wav"; { std::ofstream f(empty, std::ios::binary); } h = 0;
    expect(AveMediaBridge_ExportBegin(empty.c_str(), &spec, &h, nullptr, 0) == AMBE_OK && h != 0, "existing empty scratch opens");
    if (h) { AveMediaBridge_ExportAbort(h, nullptr, 0); AveMediaBridge_ExportDestroy(h, nullptr, 0); }
    fs::remove(empty);
    const auto abortPath = root / L"aborted.wav"; { std::ofstream f(abortPath, std::ios::binary); }
    h = 0;
    expect(AveMediaBridge_ExportBegin(abortPath.c_str(), &spec, &h, nullptr, 0) == AMBE_OK && h != 0,
        "begin scratch for public abort");
    if (h) {
        const float sample = 0.25f;
        expect(AveMediaBridge_ExportWriteF32(h, &sample, 1, nullptr, 0) == AMBE_OK,
            "write before public abort");
        expect(AveMediaBridge_ExportAbort(h, nullptr, 0) == AMBE_OK, "public abort succeeds");
        HANDLE reopened = CreateFileW(abortPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        expect(reopened != INVALID_HANDLE_VALUE, "scratch is reopenable before Destroy");
        if (reopened != INVALID_HANDLE_VALUE) CloseHandle(reopened);
        std::error_code removeError;
        expect(fs::remove(abortPath, removeError) && !removeError,
            "scratch is deletable before Destroy");
        expect(AveMediaBridge_ExportDestroy(h, nullptr, 0) == AMBE_OK, "destroy after abort");
    }
    if (fs::exists(abortPath)) fs::remove(abortPath);
    const auto directAbortPath = root / L"direct-abort.wav";
    { std::ofstream f(directAbortPath, std::ios::binary); }
    {
        AveMediaBridge::Export::ExportScratchIo io(directAbortPath);
        const uint8_t data[4] = {1,2,3,4};
        avio_write(io.context(), data, 4);
        io.abortClose();
        io.abortClose();
        HANDLE reopened = CreateFileW(directAbortPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        expect(reopened != INVALID_HANDLE_VALUE, "direct scratch abort closes idempotently");
        if (reopened != INVALID_HANDLE_VALUE) CloseHandle(reopened);
        expect(fs::file_size(directAbortPath) == 0, "direct scratch abort discards buffered PCM");
    }
    fs::remove(directAbortPath);
    // Distinct deterministic faults: disk-full and a successful short OS write.
    // Both must be latched by the real AVIO scratch boundary, then close for owner cleanup.
    for (auto fault : {AveMediaBridge::Export::ExportScratchIo::Fault::DiskFull,
                       AveMediaBridge::Export::ExportScratchIo::Fault::ShortWrite}) {
        const auto injected = static_cast<int>(fault);
        const auto path = root / (std::to_wstring(injected) + L"-write-boundary.wav");
        { std::ofstream f(path, std::ios::binary); }
        {
            AveMediaBridge::Export::ExportScratchIo io(path,
                fault);
            const uint8_t data[4] = {1,2,3,4};
            avio_write(io.context(), data, 4); avio_flush(io.context());
            expect(io.failed() && io.context()->error < 0,
                injected == 5 ? "disk-full write must fail" : "short OS write must fail");
            expect(io.context()->error == AVERROR(injected == 5 ? ENOSPC : EIO),
                "disk-full and short-write retain distinct boundary error codes");
            try { io.flushAndClose(); expect(false, "failed boundary cannot finalize"); }
            catch (const std::exception&) {}
        }
        expect(fs::file_size(path) == (injected == 5 ? 0 : 3), "distinct actual disk-full/short-write extents");
        expect(fs::remove(path), "failed writer releases scratch for owned cleanup");
    }
    for (auto fault : {AveMediaBridge::Export::ExportScratchIo::Fault::Write,
                       AveMediaBridge::Export::ExportScratchIo::Fault::Seek,
                       AveMediaBridge::Export::ExportScratchIo::Fault::Flush,
                       AveMediaBridge::Export::ExportScratchIo::Fault::Close}) {
        const auto path = root / (std::to_wstring(static_cast<int>(fault)) + L"-fault.wav");
        { std::ofstream f(path, std::ios::binary); }
        {
            AveMediaBridge::Export::ExportScratchIo io(path, fault);
            if (fault == AveMediaBridge::Export::ExportScratchIo::Fault::Write) {
                const uint8_t data[4] = {1,2,3,4}; avio_write(io.context(), data, 4); avio_flush(io.context());
                expect(io.failed() || io.context()->error, "write fault observable");
            } else if (fault == AveMediaBridge::Export::ExportScratchIo::Fault::Seek) {
                expect(avio_seek(io.context(), 4096, SEEK_SET) < 0 && io.failed(), "seek fault observable");
            } else {
                try { io.flushAndClose(); expect(false, "flush or close fault must throw"); }
                catch (const std::exception&) { }
            }
        }
        fs::remove(path);
    }
    const auto collision = root / L"collision";
    fs::create_directory(collision);
    const auto sentinel = collision / L"sentinel.txt";
    { std::ofstream f(sentinel); f << "preserve"; }
    const auto selected = acquireOwnedExportTestRoot("ambe-collision-", collision);
    std::string sentinelText;
    { std::ifstream f(sentinel); f >> sentinelText; }
    expect(selected != collision && sentinelText == "preserve",
        "colliding test root content preserved and retried");
    if (selected != collision) fs::remove_all(selected);
    fs::remove(nonempty); fs::remove_all(root); return failures ? 1 : 0;
}
