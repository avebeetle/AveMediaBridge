#include "Input/DemuxSession.hpp"
#include "Input/ReaderInputError.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
}

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using AveMediaBridge::Input::DemuxOpenOptions;
using AveMediaBridge::Input::DemuxSession;
using AveMediaBridge::Input::MediaInputSource;
using AveMediaBridge::Input::ReaderInputError;
using AveMediaBridge::Input::ReaderInputFailure;

namespace {
struct Checks {
    int count = 0;
    int failures = 0;
    void expect(bool condition, const char* name) {
        ++count;
        if (!condition) { ++failures; std::cerr << "FAIL " << name << '\n'; }
    }
};

struct Bytes {
    std::vector<uint8_t> data;
    int reads = 0;
    int checks = 0;
    int failAfterReads = -1;
    int cancelAfterReads = -1;
    int cancelAfterChecks = -1;
    bool failedOnce = false;
};

AMBI_Status __cdecl readAt(void* user, uint64_t offset, void* destination,
    uint32_t requested, uint32_t* count) {
    auto& bytes = *static_cast<Bytes*>(user);
    *count = 0;
    if (bytes.failAfterReads >= 0 && bytes.reads >= bytes.failAfterReads &&
        !bytes.failedOnce) {
        bytes.failedOnce = true;
        return AMBI_IO_ERROR;
    }
    if (offset > bytes.data.size() || requested > bytes.data.size() - offset)
        return AMBI_INVALID_ARGUMENT;
    std::memcpy(destination, bytes.data.data() + offset, requested);
    *count = requested;
    ++bytes.reads;
    return AMBI_OK;
}

AMBI_Status __cdecl checkCancel(void* user) {
    auto& bytes = *static_cast<Bytes*>(user);
    ++bytes.checks;
    if ((bytes.cancelAfterReads >= 0 && bytes.reads >= bytes.cancelAfterReads) ||
        (bytes.cancelAfterChecks >= 0 && bytes.checks >= bytes.cancelAfterChecks))
        return AMBI_CANCELED;
    return AMBI_OK;
}

AMBI_SourceV1 descriptor(Bytes& bytes) {
    AMBI_SourceV1 source{};
    source.structSize = sizeof source;
    source.abiVersion = AMBI_ABI_VERSION;
    source.byteSize = bytes.data.size();
    source.sourceToken[0] = 0x51;
    source.sourceToken[15] = 0xA7;
    source.user = &bytes;
    source.readAt = &readAt;
    source.checkCancel = &checkCancel;
    return source;
}

std::vector<uint8_t> load(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), {});
}
}

int main(int argc, char** argv) {
    Checks check;
    if (argc != 2) return 2;
    Bytes bytes{load(argv[1])};
    check.expect(bytes.data.size() > 131072, "fixture crosses two AVIO buffers");
    if (bytes.data.size() <= 131072) return 1;

    const auto path = MediaInputSource::fromPath(argv[1]);
    check.expect(!path.isStable() && path.legacyPath() &&
        *path.legacyPath() == argv[1] && path.stableSource() == nullptr,
        "path source retains path kind");
    auto original = descriptor(bytes);
    const auto expectedDescriptor = original;
    const auto stable = MediaInputSource::fromStable(original,
        "Z:/missing/reader-label-not-a-path.m4a");
    original.byteSize = 1;
    original.sourceToken[15] = 0;
    check.expect(stable.isStable() && stable.legacyPath() == nullptr &&
        stable.displayLabel() == "Z:/missing/reader-label-not-a-path.m4a" &&
        stable.stableSource() && stable.stableSource()->byteSize == bytes.data.size() &&
        stable.stableSource()->sourceToken[15] == 0xA7 &&
        stable.stableSource()->user == &bytes &&
        std::memcmp(stable.stableSource(), &expectedDescriptor,
            sizeof expectedDescriptor) == 0,
        "stable source copies descriptor and token, borrows user, retains label only");

    std::unique_ptr<DemuxSession> pathname;
    check.expect(DemuxSession::open(path, {}, pathname) == 0 && pathname &&
        pathname->get() && pathname->get()->nb_streams > 0 &&
        pathname->inputError() == 0, "automatic pathname open");
    std::unique_ptr<DemuxSession> first, second;
    DemuxOpenOptions limits{true, 262144, 1500000};
    check.expect(DemuxSession::open(stable, limits, first) == 0 && first &&
        first->get() && first->get()->iformat &&
        std::string(first->get()->iformat->name).find("mov") == 0 &&
        first->get()->probesize == limits.probeSizeBytes &&
        first->get()->max_analyze_duration == limits.analyzeDurationUs &&
        first->inputError() == 0, "stable open uses MOV and exact probe limits");
    check.expect(DemuxSession::open(stable, {}, second) == 0 && second &&
        second->get() && second->get() != first->get(),
        "independent stable sessions");
    if (first && first->get()) {
        AVIOContext* secondary = nullptr;
        check.expect(first->get()->io_open &&
            first->get()->io_open(first->get(), &secondary,
                "file:///forbidden", AVIO_FLAG_READ, nullptr) == AVERROR(EACCES) &&
            secondary == nullptr, "stable session denies file secondary open");
        check.expect(first->get()->io_open(first->get(), &secondary,
                "https://invalid.example/forbidden", AVIO_FLAG_READ, nullptr) ==
                AVERROR(EACCES) && secondary == nullptr,
            "stable session denies network secondary open");
    }

    const int originalLogLevel = av_log_get_level();
    av_log_set_level(AV_LOG_QUIET);
    Bytes fault{bytes.data};
    fault.failAfterReads = 1;
    std::unique_ptr<DemuxSession> faultSession;
    const int faultOpen = DemuxSession::open(
        MediaInputSource::fromStable(descriptor(fault), "Z:/missing/fault.m4a"),
        {}, faultSession);
    if (faultOpen == 0 && faultSession) {
        AVPacket* packet = av_packet_alloc();
        for (int i = 0; i < 10000 && faultSession->inputError() == 0; ++i) {
            if (av_read_frame(faultSession->get(), packet) < 0) break;
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
    }
    check.expect(fault.failedOnce && faultSession &&
        faultSession->inputError() == AVERROR(EIO),
        "one-shot callback failure remains terminal after valid prefix");

    Bytes canceled{bytes.data};
    canceled.cancelAfterChecks = 2;
    std::unique_ptr<DemuxSession> canceledSession;
    const int canceledOpen = DemuxSession::open(
        MediaInputSource::fromStable(descriptor(canceled), "Z:/missing/cancel.m4a"),
        {}, canceledSession);
    check.expect(canceledOpen == AVERROR_EXIT && !canceledSession &&
        canceled.checks >= 2, "cancellation during discovery rejects session");

    Bytes canceledAfterPrefix{bytes.data};
    canceledAfterPrefix.cancelAfterReads = 1;
    std::unique_ptr<DemuxSession> canceledPrefixSession;
    const int prefixOpen = DemuxSession::open(MediaInputSource::fromStable(
        descriptor(canceledAfterPrefix), "Z:/missing/canceled-prefix.m4a"),
        {}, canceledPrefixSession);
    if (prefixOpen == 0 && canceledPrefixSession) {
        AVPacket* packet = av_packet_alloc();
        for (int i = 0; i < 10000 && canceledPrefixSession->inputError() == 0; ++i) {
            if (av_read_frame(canceledPrefixSession->get(), packet) < 0) break;
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
    }
    check.expect(canceledAfterPrefix.reads >= 1 &&
        ((prefixOpen == AVERROR_EXIT && !canceledPrefixSession) ||
        (canceledPrefixSession && canceledPrefixSession->inputError() == AVERROR_EXIT)),
        "cancellation after valid prefix remains terminal");
    av_log_set_level(originalLogLevel);

    const ReaderInputError tagged(ReaderInputFailure::Canceled, "stopped");
    check.expect(tagged.failure() == ReaderInputFailure::Canceled &&
        std::string(tagged.what()) == "stopped", "private typed error retains reason");
    std::cout << "checks=" << check.count << " failures=" << check.failures << '\n';
    return check.failures ? 1 : 0;
}
