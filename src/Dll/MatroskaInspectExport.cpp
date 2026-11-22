#include "AveMediaBridge/AveMediaBridgeMatroskaInspectApi.h"
#include "Input/DemuxSession.hpp"
#include "Input/StableAvioInput.hpp"
#include "Probe/MediaProbeService.hpp"
#include "Probe/QueuedSourceSha256.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>

namespace {
using namespace AveMediaBridge;

// A private cursor on the pinned object, independent of caller and scanner.
// FFmpeg is allowed bounded metadata reads (including seeks into Cues/Tags).
struct MetadataInput {
    static constexpr std::uint64_t limit = 8U * 1024 * 1024;
    HANDLE file;
    Probe::QueuedSourceSha256& hash;
    std::uint64_t bytesRead = 0;
    bool limitReached = false;
    bool ioError = false;

    ~MetadataInput() {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }

    static AMBI_Status __cdecl read(void* object, std::uint64_t offset, void* dest,
                                    std::uint32_t count, std::uint32_t* actual) {
        auto& self = *static_cast<MetadataInput*>(object);
        *actual = 0;
        if (count > limit - self.bytesRead) {
            self.limitReached = true;
            return AMBI_IO_ERROR;
        }
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(offset);
        DWORD returned = 0;
        if (!SetFilePointerEx(self.file, pos, nullptr, FILE_BEGIN) ||
            !ReadFile(self.file, dest, count, &returned, nullptr) || returned != count) {
            self.ioError = true;
            return AMBI_IO_ERROR;
        }
        *actual = returned;
        self.bytesRead += returned;
        return AMBI_OK;
    }

    static AMBI_Status __cdecl check(void* object) {
        return static_cast<MetadataInput*>(object)->hash.canceled() ? AMBI_CANCELED : AMBI_OK;
    }
};

int selectCandidate(const std::filesystem::path& label, HANDLE pin,
                    Probe::QueuedSourceSha256& hash, std::uint64_t size,
                    Probe::MatroskaAacSequentialEligibility& eligibility) {
    MetadataInput metadata{ReOpenFile(pin, GENERIC_READ, FILE_SHARE_READ, 0), hash};
    if (metadata.file == INVALID_HANDLE_VALUE) return -2;
    AMBI_SourceV1 source{};
    source.structSize = sizeof(source);
    source.abiVersion = AMBI_ABI_VERSION;
    source.byteSize = size;
    // This token identifies this synchronous, unshared metadata session.
    const auto sessionId = reinterpret_cast<std::uintptr_t>(&metadata);
    std::memcpy(source.sourceToken, &sessionId, sizeof(sessionId));
    source.user = &metadata;
    source.readAt = &MetadataInput::read;
    source.checkCancel = &MetadataInput::check;
    std::unique_ptr<Input::StableAvioInput> input;
    if (Input::StableAvioInput::create(source, input) < 0) return -2;
    const AVInputFormat* matroska = av_find_input_format("matroska");
    if (!matroska) return -2;
    int status = input->open(matroska, {true, 4 * 1024 * 1024, 3 * AV_TIME_BASE});
    if (status >= 0) status = avformat_find_stream_info(input->format(), nullptr);
    if (hash.canceled()) return -1;
    if (metadata.ioError) return -2;
    if (metadata.limitReached || status < 0 || input->terminalError() < 0) return 0;
    eligibility = Probe::eligibleForCombinedMatroskaScan(label.u8string(), input->format());
    return eligibility.eligible ? 1 : 0;
}

bool enqueue(void* hash, const std::uint8_t* data, std::size_t size) {
    return static_cast<Probe::QueuedSourceSha256*>(hash)->append(data, size);
}
}  // namespace

extern "C" AMBM_API int __cdecl AveMediaBridge_InspectMatroskaAacSourceV1(
    const wchar_t* path, void* pinned_file, AMBM_Cancel cancel, void* context,
    uint8_t* sha256_32, uint64_t* bytes, uint64_t* frames, int32_t* stream_index,
    int32_t* sample_rate, int32_t* channels) {
    if (!path || !*path || !pinned_file || pinned_file == INVALID_HANDLE_VALUE ||
        !sha256_32 || !bytes || !frames || !stream_index || !sample_rate || !channels) return -2;
    try {
        using namespace AveMediaBridge::Probe;
        QueuedSourceSha256 hash(cancel, context);
        if (hash.canceled()) return -1;
        LARGE_INTEGER length{};
        if (GetFileType(pinned_file) != FILE_TYPE_DISK ||
            !GetFileSizeEx(pinned_file, &length) || length.QuadPart < 0) return -2;
        if (length.QuadPart == 0) return 0;
        const auto size = static_cast<std::uint64_t>(length.QuadPart);
        std::filesystem::path label(path);
        MatroskaAacSequentialEligibility candidate;
        const int selected = selectCandidate(label, pinned_file, hash, size, candidate);
        if (selected != 1) return selected;
        if (!hash.start()) return -2;
        MatroskaScanInput input{pinned_file, &hash, &enqueue};
        MatroskaAacSequentialProbeOptions options;
        options.input = &input;
        const auto scan = probeMatroskaAacSequentialPresentation(
            label.u8string(), candidate.selected, options);
        if (hash.canceled()) return -1;
        if (scan.status == MatroskaAacSequentialStatus::IoError) return -2;
        if (!scan.exact() || !scan.selectedTrackMappingValid || !scan.timestampContinuityValid ||
            !scan.allLacingValid || !scan.allRelevantCrcValid || !scan.checkedArithmeticValid ||
            !scan.reachedSegmentEnd || !scan.reachedPhysicalEof || scan.fileSizeBytes != size ||
            scan.bytesReturned != size || scan.uniqueBytes != size || scan.duplicateBytes ||
            scan.seekCallsAfterOpen || scan.initialSkipFrames || scan.terminalDiscardFrames ||
            !scan.terminalZeroProvenByCompleteAbsence || scan.presentationFrames == 0 ||
            scan.sampleRate != candidate.selected.sampleRate ||
            scan.channels != candidate.selected.channels) return 0;
        std::array<std::uint8_t, 32> digest{};
        if (!hash.finish(digest, size)) return hash.canceled() ? -1 : -2;
        if (hash.canceled()) return -1;
        std::copy(digest.begin(), digest.end(), sha256_32);
        *bytes = size;
        *frames = scan.presentationFrames;
        *stream_index = candidate.selected.streamIndex;
        *sample_rate = scan.sampleRate;
        *channels = scan.channels;
        return 1;
    } catch (...) {
        return -2;
    }
}
