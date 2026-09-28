#include "AveMediaBridge/AveMediaBridgeReaderApi.h"
#include <iostream>
#include <type_traits>
#include <stdexcept>
#define _Static_assert static_assert
#include "ReaderImportAbiC.c"
static_assert(std::is_same_v<AveMediaBridgeCancelCallback, int(__stdcall *)(void *)>);
static_assert(std::is_same_v<AveMediaBridgeProgressCallback,
                             void(__stdcall *)(const AveMediaBridgeImportProgress *, void *)>);
static_assert(std::is_same_v<AveMediaBridgeWaveformChunkCallback,
                             void(__stdcall *)(const AveMediaBridgeWaveformChunk *, void *)>);
static_assert(std::is_same_v<decltype(&AveMediaBridge_ReaderPrepareV1),
                             int(__cdecl *)(const AMBR_PrepareOptionsV1 *, AMBR_PreparedInput **)>);
AMBI_Status __cdecl throwingRead(void *, uint64_t, void *, uint32_t, uint32_t *) {
    throw std::runtime_error("injected reader exception");
}
AMBI_Status __cdecl notCanceled(void *) { return AMBI_OK; }
int main() {
    int failed = 0, checks = 0;
    auto check = [&](bool ok, const char *text) {
        ++checks;
        if (!ok) {
            ++failed;
            std::cerr << "FAIL " << text << '\n';
        }
    };
    auto *handle = reinterpret_cast<AMBR_PreparedInput *>(1);
    check(AveMediaBridge_ReaderPrepareV1(nullptr, &handle) == AMBR_INVALID_ARGUMENT,
          "null prepare result");
    check(handle == nullptr, "null prepare clears output");
    check(AveMediaBridge_ReaderPrepareV1(nullptr, nullptr) == AMBR_INVALID_ARGUMENT, "null output");
    check(AveMediaBridge_ReaderImportV1(nullptr, nullptr) == AMBR_INVALID_ARGUMENT, "null import");
    check(AveMediaBridge_ReaderWriteProbeJsonV1(nullptr, nullptr) == AMBR_INVALID_ARGUMENT,
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
        check(AveMediaBridge_ReaderPrepareV1(&options, &handle) == AMBR_INVALID_ARGUMENT,
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
        check(AveMediaBridge_ReaderPrepareV1(&options, &handle) ==
                  (mode == 7 ? AMBR_INPUT_FAILED : AMBR_INVALID_ARGUMENT),
              "descriptor validation and callback exception result");
        check(handle == nullptr, "descriptor failure returns no handle");
    }
    AveMediaBridge_ReaderDestroyV1(nullptr);
    std::cout << checks << " checks, " << failed << " failures\n";
    return failed ? 1 : 0;
}
