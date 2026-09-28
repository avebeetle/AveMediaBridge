#include "AveMediaBridge/AveMediaBridgeExportApi.h"
#include "Export/ExportScratchIo.hpp"
extern "C" {
#include <libavformat/avio.h>
}
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <windows.h>
namespace fs = std::filesystem;
int main() {
    int failures = 0; auto expect = [&](bool yes, const char* why) { if (!yes) { std::cerr << "FAIL " << why << '\n'; ++failures; } };
    const fs::path root = fs::temp_directory_path() / ("ambe-scratch-" + std::to_string(GetCurrentProcessId())); fs::create_directory(root);
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
    fs::remove(nonempty); fs::remove(root); return failures ? 1 : 0;
}
