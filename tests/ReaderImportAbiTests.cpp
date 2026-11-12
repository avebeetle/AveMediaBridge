#include "AveMediaBridge/AveMediaBridgeReaderApi.h"

#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#define _Static_assert static_assert
#include "ReaderImportAbiC.c"
static_assert(std::is_same_v<AveMediaBridgeCancelCallback, int(__stdcall *)(void *)>);
static_assert(std::is_same_v<AveMediaBridgeProgressCallback,
                             void(__stdcall *)(const AveMediaBridgeImportProgress *, void *)>);
static_assert(std::is_same_v<AveMediaBridgeWaveformChunkCallback,
                             void(__stdcall *)(const AveMediaBridgeWaveformChunk *, void *)>);
static_assert(std::is_same_v<decltype(&AveMediaBridge_ReaderPrepareV1),
                             int(__cdecl *)(const AMBR_PrepareOptionsV1 *, AMBR_PreparedInput **)>);

namespace fs = std::filesystem;
using namespace std::chrono_literals;

struct Checks {
    int checks = 0;
    int failed = 0;

    void operator()(bool ok, const char* text) {
        ++checks;
        if (!ok) {
            ++failed;
            std::cerr << "FAIL " << text << '\n';
        }
    }
};

struct ReaderApi {
    decltype(&AveMediaBridge_ReaderPrepareV1) prepare;
    decltype(&AveMediaBridge_ReaderWriteProbeJsonV1) probe;
    decltype(&AveMediaBridge_ReaderImportV1) import;
    decltype(&AveMediaBridge_ReaderDestroyV1) destroy;
};

AMBI_Status __cdecl throwingRead(void *, uint64_t, void *, uint32_t, uint32_t *) {
    throw std::runtime_error("injected reader exception");
}
AMBI_Status __cdecl notCanceled(void *) { return AMBI_OK; }

void checkInvalidArguments(const ReaderApi& api, Checks& check) {
    auto *handle = reinterpret_cast<AMBR_PreparedInput *>(1);
    check(api.prepare(nullptr, &handle) == AMBR_INVALID_ARGUMENT,
          "null prepare result");
    check(handle == nullptr, "null prepare clears output");
    check(api.prepare(nullptr, nullptr) == AMBR_INVALID_ARGUMENT, "null output");
    check(api.import(nullptr, nullptr) == AMBR_INVALID_ARGUMENT, "null import");
    check(api.probe(nullptr, nullptr) == AMBR_INVALID_ARGUMENT,
          "null probe");
    AMBR_PrepareOptionsV1 options{};
    for (int mode = 0; mode < 4; ++mode) {
        options = {};
        options.structSize = sizeof options;
        options.abiVersion = AMBR_ABI_VERSION;
        if (mode == 0)
            options.structSize--;
        if (mode == 1)
            options.abiVersion++;
        if (mode == 2)
            options.reserved[3] = 1;
        handle = reinterpret_cast<AMBR_PreparedInput *>(1);
        check(api.prepare(&options, &handle) == AMBR_INVALID_ARGUMENT,
              "invalid options");
        check(handle == nullptr, "invalid options clears output");
    }
    for (int mode = 0; mode < 8; ++mode) {
        AMBI_SourceV1 source{};
        source.structSize = sizeof source;
        source.abiVersion = 1;
        source.byteSize = 128;
        source.sourceToken[0] = 1;
        source.readAt = throwingRead;
        source.checkCancel = notCanceled;
        if (mode == 0) source.structSize--;
        if (mode == 1) source.abiVersion++;
        if (mode == 2) source.reserved[1] = 1;
        if (mode == 3) source.sourceToken[0] = 0;
        if (mode == 4) source.byteSize = 0;
        if (mode == 5) source.readAt = nullptr;
        if (mode == 6) source.checkCancel = nullptr;
        options = {};
        options.structSize = sizeof options;
        options.abiVersion = 1;
        options.source = &source;
        handle = reinterpret_cast<AMBR_PreparedInput *>(1);
        check(api.prepare(&options, &handle) ==
                  (mode == 7 ? AMBR_INPUT_FAILED : AMBR_INVALID_ARGUMENT),
              "descriptor validation and callback exception result");
        check(handle == nullptr, "descriptor failure returns no handle");
    }
    api.destroy(nullptr);
}

struct ImportGate {
    std::promise<void> entered;
    std::shared_future<void> resume;
};

struct ReaderFixture {
    ReaderFixture(const std::vector<char>& source, const fs::path& pcm)
        : bytes(source), pcmPath(pcm) {}

    const std::vector<char>& bytes;
    fs::path pcmPath;
    ImportGate* gate = nullptr;
    std::atomic<bool> cancel{false};
    std::atomic<unsigned> callbacks{0};
    uint64_t frames = 0;
    uint64_t waveformFrames = 0;
    bool progressValid = true;
    bool waveformValid = true;
    bool throwWaveform = false;
};

AMBI_Status __cdecl fixtureRead(void* user, uint64_t offset, void* destination,
                               uint32_t requested, uint32_t* count) {
    auto& fixture = *static_cast<ReaderFixture*>(user);
    ++fixture.callbacks;
    if (offset > fixture.bytes.size() || requested > fixture.bytes.size() - offset) {
        return AMBI_IO_ERROR;
    }
    std::memcpy(destination, fixture.bytes.data() + offset, requested);
    *count = requested;
    return AMBI_OK;
}

AMBI_Status __cdecl fixtureCancel(void* user) {
    auto& fixture = *static_cast<ReaderFixture*>(user);
    ++fixture.callbacks;
    return fixture.cancel ? AMBI_CANCELED : AMBI_OK;
}

void __stdcall fixtureProgress(const AveMediaBridgeImportProgress* value, void* user) {
    auto& fixture = *static_cast<ReaderFixture*>(user);
    ++fixture.callbacks;
    if (!value || value->structSize != sizeof(*value) || value->framesWritten < fixture.frames ||
        value->channels <= 0 || value->sampleRate <= 0) {
        fixture.progressValid = false;
        return;
    }
    if (value->framesWritten == 0) return;
    std::error_code error;
    const auto extent = fs::file_size(fixture.pcmPath, error);
    fixture.progressValid &= !error && extent >= value->bytesWritten &&
        value->bytesWritten == value->framesWritten * value->channels * sizeof(float);
    const bool firstPcm = fixture.frames == 0;
    fixture.frames = value->framesWritten;
    if (firstPcm && fixture.gate) {
        fixture.gate->entered.set_value();
        fixture.gate->resume.wait();
    }
}

void __stdcall fixtureWaveform(const AveMediaBridgeWaveformChunk* value, void* user) {
    auto& fixture = *static_cast<ReaderFixture*>(user);
    ++fixture.callbacks;
    if (fixture.throwWaveform) throw std::runtime_error("injected waveform exception");
    if (!value || value->structSize != sizeof(*value) ||
        value->firstFrame != fixture.waveformFrames || value->valuesPerBin != 2 ||
        value->binCount == 0 || !value->minMaxPairs || !value->sumSquaresPerBin ||
        !value->sumAbsPerBin || !value->frameCountPerBin) {
        fixture.waveformValid = false;
        return;
    }
    // Read the borrowed arrays only inside the callback and retain only values.
    for (uint32_t i = 0; i < value->binCount; ++i) {
        const auto minimum = value->minMaxPairs[2 * i];
        const auto maximum = value->minMaxPairs[2 * i + 1];
        const auto frames = value->frameCountPerBin[i];
        fixture.waveformValid &= std::isfinite(minimum) && std::isfinite(maximum) &&
            minimum <= maximum && std::isfinite(value->sumSquaresPerBin[i]) &&
            std::isfinite(value->sumAbsPerBin[i]) && frames > 0 && frames <= value->framesPerBin;
        fixture.waveformFrames += frames;
    }
}

constexpr wchar_t kLabel[] = L"reader-abi-borrowed-source-label.mp4";

AMBR_PreparedInput* prepareFixture(const ReaderApi& api, ReaderFixture& fixture, Checks& check) {
    auto source = std::make_unique<AMBI_SourceV1>();
    source->structSize = sizeof(*source);
    source->abiVersion = AMBI_ABI_VERSION;
    source->byteSize = fixture.bytes.size();
    source->sourceToken[0] = 1;
    source->user = &fixture;
    source->readAt = fixtureRead;
    source->checkCancel = fixtureCancel;
    const std::wstring label(kLabel);
    const AMBR_PrepareOptionsV1 options{sizeof(options), AMBR_ABI_VERSION, source.get(), label.c_str()};
    AMBR_PreparedInput* handle = nullptr;
    check(api.prepare(&options, &handle) == AMBR_OK && handle && fixture.callbacks > 0,
        "valid fixture prepares with live callbacks");
    return handle;
}

enum class ImportCase { Success, Cancel, ThrowWaveform };

void checkFixture(const ReaderApi& api, const fs::path& dir, const std::vector<char>& bytes,
                  ImportCase importCase, Checks& check) {
    fs::create_directories(dir);
    auto fixture = std::make_unique<ReaderFixture>(bytes, dir / "original_f32.bin");
    std::unique_ptr<AMBR_PreparedInput, decltype(api.destroy)> handle(
        prepareFixture(api, *fixture, check), api.destroy);
    if (!handle) return;

    const auto probe = dir / "probe.json";
    const auto afterPrepare = fixture->callbacks.load();
    check(api.probe(handle.get(), probe.c_str()) == AMBR_OK, "probe with copied label and descriptor");
    std::ifstream json(probe, std::ios::binary);
    const std::string probeText((std::istreambuf_iterator<char>(json)), {});
    check(probeText.find("\"sourcePath\": \"reader-abi-borrowed-source-label.mp4\"") != std::string::npos,
        "probe retains caller label after it is freed");
    check(fixture->callbacks == afterPrepare, "probe does not redispatch source callbacks");

    AMBR_ImportOptionsV1 options{};
    options.structSize = sizeof(options);
    options.abiVersion = AMBR_ABI_VERSION;
    options.sessionMediaDir = dir.c_str();
    options.onProgress = fixtureProgress;
    options.onWaveformChunk = fixtureWaveform;
    options.userData = fixture.get();
    auto invalid = options;
    invalid.reserved[0] = 1;
    check(api.import(handle.get(), &invalid) == AMBR_INVALID_ARGUMENT &&
        fixture->callbacks == afterPrepare, "invalid import does not consume handle or dispatch");

    fixture->throwWaveform = importCase == ImportCase::ThrowWaveform;
    int status;
    if (importCase == ImportCase::Cancel) {
        std::promise<void> releaseImport;
        ImportGate gate{{}, releaseImport.get_future().share()};
        auto entered = gate.entered.get_future();
        fixture->gate = &gate;
        auto running = std::async(std::launch::async, [&] { return api.import(handle.get(), &options); });
        const bool inProgress = entered.wait_for(5s) == std::future_status::ready;
        check(inProgress && running.wait_for(0s) == std::future_status::timeout,
            "worker is held inside committed PCM progress");
        fixture->cancel = true;
        releaseImport.set_value();
        status = running.get();
        fixture->gate = nullptr;
    } else {
        status = api.import(handle.get(), &options);
    }
    const auto afterImport = fixture->callbacks.load();
    check(afterImport > afterPrepare && fixture->frames > 0 && fixture->progressValid,
        "joined import used the copied descriptor and published the announced PCM extent");
    if (importCase == ImportCase::Success) {
        check(status == AMBR_OK && fixture->waveformValid && fixture->waveformFrames == fixture->frames,
            "successful import consumes borrowed waveform buffers during callbacks");
    } else {
        const int expected = importCase == ImportCase::Cancel ? AMBR_CANCELED : AMBR_INPUT_FAILED;
        check(status == expected && !fs::exists(fixture->pcmPath),
            "cancellation or callback exception returns expected status and removes partial PCM");
    }
    check(api.import(handle.get(), &options) == AMBR_WRONG_STATE && fixture->callbacks == afterImport,
        "serialized retry after return is rejected without callbacks");
    handle.reset();
    check(fixture->callbacks == afterImport, "destroy after return does not call the live owner");
    fixture.reset();
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 5) return 2;
    Checks check;
    const fs::path dll(argv[1]);
    check(GetModuleHandleW(dll.filename().c_str()) == nullptr, "host does not implicitly load Bridge");
    HMODULE module = LoadLibraryExW(dll.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) {
        std::cerr << "Bridge load failed with Win32 error " << GetLastError() << '\n';
        return 1;
    }
    const ReaderApi api{
        reinterpret_cast<decltype(api.prepare)>(GetProcAddress(module, "AveMediaBridge_ReaderPrepareV1")),
        reinterpret_cast<decltype(api.probe)>(GetProcAddress(module, "AveMediaBridge_ReaderWriteProbeJsonV1")),
        reinterpret_cast<decltype(api.import)>(GetProcAddress(module, "AveMediaBridge_ReaderImportV1")),
        reinterpret_cast<decltype(api.destroy)>(GetProcAddress(module, "AveMediaBridge_ReaderDestroyV1"))};
    check(api.prepare && api.probe && api.import && api.destroy, "reader C ABI resolves");
    if (api.prepare && api.probe && api.import && api.destroy) {
        checkInvalidArguments(api, check);
        if (*argv[4]) {
            std::ifstream input(fs::path(argv[4]), std::ios::binary);
            const std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
            check(!bytes.empty(), "generated MOV/AAC fixture is readable");
            if (!bytes.empty()) {
                const auto root = fs::path(argv[3]) /
                    (std::to_string(GetCurrentProcessId()) + "-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
                checkFixture(api, root / "success", bytes, ImportCase::Success, check);
                checkFixture(api, root / "cancel", bytes, ImportCase::Cancel, check);
                checkFixture(api, root / "throw-waveform", bytes, ImportCase::ThrowWaveform, check);
            }
        } else {
            std::cout << "Reader lifetime fixture not configured\n";
        }
    }
    if (std::wstring(argv[2]) == L"unload") {
        check(FreeLibrary(module) != 0 && GetModuleHandleW(dll.filename().c_str()) == nullptr,
            "Bridge module unloads after every handle and callback owner is released");
    }
    std::cout << check.checks << " checks, " << check.failed << " failures\n";
    return check.failed ? 1 : 0;
}
