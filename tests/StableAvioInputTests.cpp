#include "Input/StableAvioInput.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
}

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using AveMediaBridge::Input::StableAvioInput;

char capturedFfmpegLog[4096]{};
size_t capturedFfmpegLogSize = 0;
int ffmpegLogPrefix = 1;

void captureFfmpegLog(void* context, int level, const char* format, va_list arguments) {
    char line[1024]{};
    av_log_format_line2(context, level, format, arguments, line, sizeof(line),
        &ffmpegLogPrefix);
    const size_t available = sizeof(capturedFfmpegLog) - capturedFfmpegLogSize - 1;
    const size_t length = std::min(std::strlen(line), available);
    std::memcpy(capturedFfmpegLog + capturedFfmpegLogSize, line, length);
    capturedFfmpegLogSize += length;
    capturedFfmpegLog[capturedFfmpegLogSize] = '\0';
}

struct Checks {
    int failures = 0;
    int cases = 0;
    void expect(bool ok, const char* name) {
        ++cases;
        if (!ok) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
    }
};

struct Bytes {
    enum class Fault { None, ShortOk, IoError, Throw };
    std::vector<uint8_t> data;
    std::atomic<bool> canceled{false};
    std::atomic<int> reads{0};
    std::atomic<int> checks{0};
    std::atomic<int> maxRequest{0};
    std::atomic<int> cancelAfterReads{-1};
    std::atomic<int> errorAfterReads{-1};
    std::atomic<int> cancelOnceAfterReads{-1};
    std::atomic<int> errorOnceAfterReads{-1};
    Fault fault = Fault::None;
};

AMBI_Status __cdecl readBytes(void* user, uint64_t offset, void* destination,
    uint32_t count, uint32_t* actual) {
    auto& b = *static_cast<Bytes*>(user);
    const int nth = ++b.reads;
    b.maxRequest.store(std::max(b.maxRequest.load(), static_cast<int>(count)));
    if (b.errorAfterReads.load() >= 0 && nth > b.errorAfterReads.load())
        return AMBI_IO_ERROR;
    const int oneShotError = b.errorOnceAfterReads.load();
    if (oneShotError >= 0 && nth > oneShotError) {
        b.errorOnceAfterReads = -1;
        return AMBI_IO_ERROR;
    }
    if (b.fault == Bytes::Fault::Throw) throw std::runtime_error("read fault");
    if (b.fault == Bytes::Fault::IoError) return AMBI_IO_ERROR;
    if (b.fault == Bytes::Fault::ShortOk) {
        *actual = count - 1;
        return AMBI_OK;
    }
    if (offset > b.data.size() || count > b.data.size() - offset) return AMBI_IO_ERROR;
    std::memcpy(destination, b.data.data() + offset, count);
    *actual = count;
    if (b.cancelAfterReads.load() == nth) b.canceled = true;
    return AMBI_OK;
}

AMBI_Status __cdecl checkBytes(void* user) {
    auto& b = *static_cast<Bytes*>(user);
    ++b.checks;
    const int oneShotCancel = b.cancelOnceAfterReads.load();
    if (oneShotCancel >= 0 && b.reads.load() >= oneShotCancel) {
        b.cancelOnceAfterReads = -1;
        return AMBI_CANCELED;
    }
    return b.canceled ? AMBI_CANCELED : AMBI_OK;
}

AMBI_SourceV1 sourceFor(Bytes& b) {
    AMBI_SourceV1 source{};
    source.structSize = sizeof(source);
    source.abiVersion = AMBI_ABI_VERSION;
    source.byteSize = b.data.size();
    source.sourceToken[0] = 1;
    source.user = &b;
    source.readAt = readBytes;
    source.checkCancel = checkBytes;
    return source;
}

std::vector<uint8_t> sequence(size_t count) {
    std::vector<uint8_t> result(count);
    for (size_t i = 0; i < count; ++i) result[i] = static_cast<uint8_t>((i * 17 + 3) & 255);
    return result;
}

void put16(std::vector<uint8_t>& result, uint16_t x) {
    result.push_back(static_cast<uint8_t>(x));
    result.push_back(static_cast<uint8_t>(x >> 8));
}
void put32(std::vector<uint8_t>& result, uint32_t x) {
    put16(result, static_cast<uint16_t>(x));
    put16(result, static_cast<uint16_t>(x >> 16));
}
std::vector<uint8_t> makeWav(const std::vector<uint8_t>& pcm) {
    std::vector<uint8_t> wav;
    auto tag = [&](const char* four) { wav.insert(wav.end(), four, four + 4); };
    tag("RIFF"); put32(wav, static_cast<uint32_t>(36 + pcm.size())); tag("WAVE");
    tag("fmt "); put32(wav, 16); put16(wav, 1); put16(wav, 1);
    put32(wav, 8000); put32(wav, 16000); put16(wav, 2); put16(wav, 16);
    tag("data"); put32(wav, static_cast<uint32_t>(pcm.size()));
    wav.insert(wav.end(), pcm.begin(), pcm.end());
    return wav;
}

int main() {
    Checks check;
    Bytes bytes; bytes.data = sequence(128);
    auto source = sourceFor(bytes);
    std::unique_ptr<StableAvioInput> a, b;
    check.expect(StableAvioInput::create(source, a) == 0 && a && a->io(),
        "create first AVIO");
    if (!a) return 1;
    check.expect(StableAvioInput::create(source, b) == 0 && b && b->io(),
        "create second AVIO");
    if (!b) return 1;
    source.user = nullptr;
    source.readAt = nullptr;
    uint8_t first[16]{}, second[16]{};
    check.expect(avio_read(a->io(), first, 8) == 8 &&
        std::memcmp(first, bytes.data.data(), 8) == 0, "first independent read");
    check.expect(avio_seek(b->io(), 32, SEEK_SET) == 32 &&
        avio_read(b->io(), second, 8) == 8 &&
        std::memcmp(second, bytes.data.data() + 32, 8) == 0,
        "second independent seek and read");
    check.expect(avio_read(a->io(), first, 8) == 8 &&
        std::memcmp(first, bytes.data.data() + 8, 8) == 0,
        "first cursor unaffected by second");
    check.expect(avio_seek(a->io(), 4, SEEK_SET) == 4 &&
        avio_seek(a->io(), 3, SEEK_CUR) == 7,
        "FFmpeg set and cur seeks");
    check.expect(a->io()->seek(a->io()->opaque, -2, SEEK_END) == 126,
        "callback seek end uses fixed size");
    const int64_t before = a->io()->seek(a->io()->opaque, 0, SEEK_CUR);
    const int readsBeforeSize = bytes.reads;
    check.expect(a->io()->seek(a->io()->opaque, 0, AVSEEK_SIZE) == 128 &&
        a->io()->seek(a->io()->opaque, 0, SEEK_CUR) == before &&
        bytes.reads == readsBeforeSize, "size query preserves cursor without read");
    check.expect(a->io()->seek(a->io()->opaque, -1, SEEK_SET) == AVERROR(EINVAL) &&
        a->io()->seek(a->io()->opaque, 0, SEEK_CUR) == before,
        "negative absolute seek rejected");
    check.expect(a->io()->seek(a->io()->opaque, 129, SEEK_SET) == AVERROR(EINVAL) &&
        a->io()->seek(a->io()->opaque, 0, SEEK_CUR) == before,
        "past-end seek rejected");
    check.expect(a->io()->seek(a->io()->opaque, 2, SEEK_CUR) == 128 &&
        a->io()->seek(a->io()->opaque, 0, SEEK_CUR) == 128,
        "direct cursor tracks checked end");
    check.expect(a->io()->seek(a->io()->opaque, 0, AVSEEK_SIZE | AVSEEK_FORCE) == 128 &&
        a->io()->seek(a->io()->opaque, 0, SEEK_SET | AVSEEK_FORCE) == 0 &&
        a->io()->seek(a->io()->opaque, 0, 0x1000) == AVERROR(EINVAL),
        "forced size/seek and unknown whence");
    check.expect(a->io()->seek(a->io()->opaque, 128, SEEK_SET) == 128 &&
        a->io()->read_packet(a->io()->opaque, first, 1) == AVERROR_EOF,
        "seek exactly EOF then read");
    bytes.canceled = true;
    check.expect(a->io()->seek(a->io()->opaque, 0, AVSEEK_SIZE) == AVERROR_EXIT,
        "size query observes cancellation");
    check.expect(a->io()->read_packet(a->io()->opaque, first, 1) == AVERROR_EXIT,
        "read observes cancellation");
    check.expect(b->io()->read_packet(b->io()->opaque, second, 1) == AVERROR_EXIT,
        "shared canceled source affects second reader");
    bytes.canceled = false;
    std::unique_ptr<StableAvioInput> afterCancel;
    check.expect(StableAvioInput::create(sourceFor(bytes), afterCancel) == 0 && afterCancel,
        "new reader after canceled reader remains available");
    if (!afterCancel) return 1;
    check.expect(afterCancel->io()->seek(afterCancel->io()->opaque, 126, SEEK_SET) == 126 &&
        afterCancel->io()->read_packet(afterCancel->io()->opaque, first, 8) == 2 &&
        std::memcmp(first, bytes.data.data() + 126, 2) == 0 &&
        afterCancel->io()->read_packet(afterCancel->io()->opaque, first, 1) == AVERROR_EOF,
        "EOF returns remaining bytes then EOF");
    bytes.fault = Bytes::Fault::ShortOk;
    check.expect(afterCancel->io()->seek(afterCancel->io()->opaque, 0, SEEK_SET) == 0 &&
        afterCancel->io()->read_packet(afterCancel->io()->opaque, first, 4) == AVERROR(EIO),
        "short OK is I/O error");
    bytes.fault = Bytes::Fault::IoError;
    std::unique_ptr<StableAvioInput> errorReader;
    check.expect(StableAvioInput::create(sourceFor(bytes), errorReader) == 0 &&
        errorReader && errorReader->io()->read_packet(errorReader->io()->opaque, first, 4) ==
            AVERROR(EIO),
        "callback I/O error");
    bytes.fault = Bytes::Fault::Throw;
    std::unique_ptr<StableAvioInput> throwReader;
    check.expect(StableAvioInput::create(sourceFor(bytes), throwReader) == 0 &&
        throwReader && throwReader->io()->read_packet(throwReader->io()->opaque, first, 4) ==
            AVERROR(EIO),
        "callback exception");
    bytes.fault = Bytes::Fault::None;

    Bytes huge; huge.data = sequence(5 * 1024 * 1024 + 31);
    std::unique_ptr<StableAvioInput> large;
    check.expect(StableAvioInput::create(sourceFor(huge), large) == 0 && large,
        "create large AVIO");
    if (large) {
        std::vector<uint8_t> output(5 * 1024 * 1024);
        check.expect(avio_read(large->io(), output.data(), static_cast<int>(output.size())) ==
            static_cast<int>(output.size()) &&
            std::memcmp(output.data(), huge.data.data(), output.size()) == 0 &&
            huge.maxRequest <= AMBI_MAX_READ_BYTES, "5 MiB read uses bounded exact portions");
        uint8_t tail[64]{};
        check.expect(avio_read(large->io(), tail, 64) == 31 &&
            std::memcmp(tail, huge.data.data() + output.size(), 31) == 0 &&
            avio_read(large->io(), tail, 1) == AVERROR_EOF,
            "large AVIO crossing EOF returns prefix then EOF");
    }
    Bytes canceledHuge; canceledHuge.data = sequence(5 * 1024 * 1024);
    canceledHuge.cancelAfterReads = 1;
    std::unique_ptr<StableAvioInput> cancelInput;
    check.expect(StableAvioInput::create(sourceFor(canceledHuge), cancelInput) == 0 &&
        cancelInput, "create cancelable AVIO");
    if (cancelInput) {
        std::vector<uint8_t> output(5 * 1024 * 1024);
        const int prefix = avio_read(cancelInput->io(), output.data(), static_cast<int>(output.size()));
        check.expect(prefix > 0 && prefix <= AMBI_MAX_READ_BYTES &&
            std::memcmp(output.data(), canceledHuge.data.data(), prefix) == 0 &&
            cancelInput->io()->error == AVERROR_EXIT &&
            avio_read(cancelInput->io(), output.data(), 1) == AVERROR_EXIT,
            "cancellation after prefix remains visible");
    }
    Bytes faultHuge; faultHuge.data = sequence(5 * 1024 * 1024);
    faultHuge.errorAfterReads = 1;
    std::unique_ptr<StableAvioInput> faultInput;
    check.expect(StableAvioInput::create(sourceFor(faultHuge), faultInput) == 0 &&
        faultInput, "create faulting large AVIO");
    if (faultInput) {
        std::vector<uint8_t> output(5 * 1024 * 1024);
        const int prefix = avio_read(faultInput->io(), output.data(), static_cast<int>(output.size()));
        check.expect(prefix > 0 && prefix <= AMBI_MAX_READ_BYTES &&
            std::memcmp(output.data(), faultHuge.data.data(), prefix) == 0 &&
            faultInput->io()->error == AVERROR(EIO) &&
            avio_read(faultInput->io(), output.data(), 1) == AVERROR(EIO),
            "I/O fault after prefix remains visible");
    }
    Bytes oneShotFault; oneShotFault.data = sequence(5 * 1024 * 1024);
    oneShotFault.errorOnceAfterReads = 1;
    std::unique_ptr<StableAvioInput> oneShotFaultInput;
    check.expect(StableAvioInput::create(sourceFor(oneShotFault), oneShotFaultInput) == 0 &&
        oneShotFaultInput, "create transient-fault AVIO");
    if (oneShotFaultInput) {
        std::vector<uint8_t> output(5 * 1024 * 1024);
        const int prefix = avio_read(oneShotFaultInput->io(), output.data(),
            static_cast<int>(output.size()));
        check.expect(prefix == AMBI_MAX_READ_BYTES &&
            oneShotFaultInput->io()->error == AVERROR(EIO),
            "transient I/O fault leaves a valid prefix");
        check.expect(avio_read(oneShotFaultInput->io(), output.data(), 131072) == AVERROR(EIO),
            "large read cannot bypass retained transient I/O fault");
        std::unique_ptr<StableAvioInput> freshReader;
        check.expect(StableAvioInput::create(sourceFor(oneShotFault), freshReader) == 0 &&
            freshReader && avio_read(freshReader->io(), output.data(), 131072) == 131072 &&
            std::memcmp(output.data(), oneShotFault.data.data(), 131072) == 0,
            "terminal error is private to faulting AVIO instance");
    }
    Bytes oneShotCancel; oneShotCancel.data = sequence(5 * 1024 * 1024);
    oneShotCancel.cancelOnceAfterReads = 1;
    std::unique_ptr<StableAvioInput> oneShotCancelInput;
    check.expect(StableAvioInput::create(sourceFor(oneShotCancel), oneShotCancelInput) == 0 &&
        oneShotCancelInput, "create transient-cancel AVIO");
    if (oneShotCancelInput) {
        std::vector<uint8_t> output(5 * 1024 * 1024);
        const int prefix = avio_read(oneShotCancelInput->io(), output.data(),
            static_cast<int>(output.size()));
        check.expect(prefix == AMBI_MAX_READ_BYTES &&
            oneShotCancelInput->io()->error == AVERROR_EXIT,
            "transient cancellation leaves a valid prefix");
        check.expect(avio_read(oneShotCancelInput->io(), output.data(), 131072) == AVERROR_EXIT,
            "large read cannot bypass retained transient cancellation");
        std::unique_ptr<StableAvioInput> freshReader;
        check.expect(StableAvioInput::create(sourceFor(oneShotCancel), freshReader) == 0 &&
            freshReader && avio_read(freshReader->io(), output.data(), 131072) == 131072 &&
            std::memcmp(output.data(), oneShotCancel.data.data(), 131072) == 0,
            "terminal cancellation is private to canceled AVIO instance");
    }
    Bytes independent; independent.data = sequence(64);
    std::unique_ptr<StableAvioInput> unaffected;
    check.expect(StableAvioInput::create(sourceFor(independent), unaffected) == 0 &&
        unaffected && unaffected->io()->read_packet(unaffected->io()->opaque, first, 4) == 4,
        "canceled source does not affect independent source");
    Bytes virtualHuge; virtualHuge.data = sequence(1);
    auto hugeSource = sourceFor(virtualHuge);
    hugeSource.byteSize = static_cast<uint64_t>(INT64_MAX);
    std::unique_ptr<StableAvioInput> overflow;
    check.expect(StableAvioInput::create(hugeSource, overflow) == 0 && overflow &&
        overflow->io()->seek(overflow->io()->opaque, INT64_MAX, SEEK_SET) == INT64_MAX &&
        overflow->io()->seek(overflow->io()->opaque, 1, SEEK_CUR) == AVERROR(EINVAL) &&
        overflow->io()->seek(overflow->io()->opaque, 0, SEEK_CUR) == INT64_MAX,
        "signed seek addition overflow preserves cursor");
    Bytes wavBytes; const auto pcm = sequence(128); wavBytes.data = makeWav(pcm);
    std::unique_ptr<StableAvioInput> wav;
    check.expect(StableAvioInput::create(sourceFor(wavBytes), wav) == 0 && wav,
        "create generated WAV reader");
    if (wav) {
        check.expect(wav->open(av_find_input_format("wav")) == 0 && wav->format(),
            "open generated WAV");
        if (wav->format()) {
            AVPacket* packet = av_packet_alloc();
            const int read = av_read_frame(wav->format(), packet);
            check.expect(read == 0 && packet->size == static_cast<int>(pcm.size()) &&
                std::memcmp(packet->data, pcm.data(), pcm.size()) == 0,
                "WAV demux packet preserves PCM payload");
            av_packet_free(&packet);
            AVIOContext* secondary = nullptr;
            check.expect(wav->format()->io_open(wav->format(), &secondary,
                "file:///should-not-open", AVIO_FLAG_READ, nullptr) == AVERROR(EACCES) &&
                secondary == nullptr, "deny file secondary open");
            check.expect(wav->format()->io_open(wav->format(), &secondary,
                "https://invalid.example/test", AVIO_FLAG_READ, nullptr) == AVERROR(EACCES) &&
                secondary == nullptr, "deny network secondary open");
        }
        check.expect(wav->open(av_find_input_format("wav")) == AVERROR(EINVAL),
            "repeat successful open rejected");
    }
    Bytes malformed; malformed.data = sequence(64);
    std::unique_ptr<StableAvioInput> bad;
    check.expect(StableAvioInput::create(sourceFor(malformed), bad) == 0 && bad,
        "create malformed reader");
    if (bad) {
        capturedFfmpegLogSize = 0;
        capturedFfmpegLog[0] = '\0';
        ffmpegLogPrefix = 1;
        av_log_set_callback(&captureFfmpegLog);
        const int malformedResult = bad->open(av_find_input_format("wav"));
        av_log_set_callback(av_log_default_callback);
        check.expect(malformedResult < 0,
            "malformed bytes fail demux");
        const std::string diagnostic(capturedFfmpegLog, capturedFfmpegLogSize);
        const bool expectedDiagnostic = diagnostic.find("invalid start code") !=
            std::string::npos && diagnostic.find("in RIFF header") !=
            std::string::npos && std::count(diagnostic.begin(), diagnostic.end(), '\n') == 1;
        if (!expectedDiagnostic) std::cerr << "Captured FFmpeg diagnostic: " << diagnostic;
        check.expect(expectedDiagnostic, "only expected malformed-WAV diagnostic captured");
        check.expect(bad->open(av_find_input_format("wav")) == AVERROR(EINVAL),
            "repeat failed open rejected");
    }
    Bytes canceledOpen; canceledOpen.data = makeWav(pcm); canceledOpen.canceled = true;
    std::unique_ptr<StableAvioInput> canceledFormat;
    check.expect(StableAvioInput::create(sourceFor(canceledOpen), canceledFormat) == 0 &&
        canceledFormat && canceledFormat->open(av_find_input_format("wav")) == AVERROR_EXIT,
        "canceled open fails");
    if (canceledFormat) check.expect(canceledFormat->open(av_find_input_format("wav")) ==
        AVERROR(EINVAL), "repeat canceled open rejected");
    Bytes lifetime; lifetime.data = makeWav(pcm);
    for (int i = 0; i < 100; ++i) {
        std::unique_ptr<StableAvioInput> item;
        if (StableAvioInput::create(sourceFor(lifetime), item) != 0 || !item ||
            item->open(av_find_input_format("wav")) != 0) {
            check.expect(false, "100 lifecycle opens");
            break;
        }
        AVPacket* packet = av_packet_alloc();
        const bool read = av_read_frame(item->format(), packet) == 0;
        av_packet_free(&packet);
        if (!read) { check.expect(false, "100 lifecycle reads"); break; }
    }
    check.expect(lifetime.reads > 0 && lifetime.checks > 0,
        "lifecycle callbacks used live source");
    auto invalid = sourceFor(lifetime);
    invalid.readAt = nullptr;
    std::unique_ptr<StableAvioInput> invalidInput;
    const bool firstValid = StableAvioInput::create(sourceFor(lifetime), invalidInput) == 0 &&
        invalidInput;
    check.expect(firstValid &&
        StableAvioInput::create(invalid, invalidInput) == AVERROR(EINVAL) &&
        !invalidInput, "invalid source clears output");
    std::unique_ptr<StableAvioInput> nullFormat;
    check.expect(StableAvioInput::create(sourceFor(lifetime), nullFormat) == 0 &&
        nullFormat && nullFormat->open(nullptr) == AVERROR(EINVAL) &&
        nullFormat->open(av_find_input_format("wav")) == AVERROR(EINVAL),
        "null format rejected and consumes open attempt");
    AVInputFormat noFile{};
    noFile.flags = AVFMT_NOFILE;
    std::unique_ptr<StableAvioInput> noFileInput;
    check.expect(StableAvioInput::create(sourceFor(lifetime), noFileInput) == 0 &&
        noFileInput && noFileInput->open(&noFile) == AVERROR(EINVAL),
        "NOFILE demuxer rejected");

    Bytes threaded; threaded.data = sequence(1024 * 1024);
    std::unique_ptr<StableAvioInput> threadA, threadB;
    const bool createdPair = StableAvioInput::create(sourceFor(threaded), threadA) == 0 &&
        StableAvioInput::create(sourceFor(threaded), threadB) == 0;
    bool exactA = false, exactB = false;
    if (createdPair) {
        auto readPart = [&](StableAvioInput* input, size_t at, bool& exact) {
            std::vector<uint8_t> data(32768);
            exact = avio_seek(input->io(), static_cast<int64_t>(at), SEEK_SET) ==
                static_cast<int64_t>(at) &&
                avio_read(input->io(), data.data(), static_cast<int>(data.size())) ==
                static_cast<int>(data.size()) &&
                std::memcmp(data.data(), threaded.data.data() + at, data.size()) == 0;
        };
        std::thread one(readPart, threadA.get(), 700000, std::ref(exactA));
        std::thread two(readPart, threadB.get(), 100000, std::ref(exactB));
        one.join(); two.join();
    }
    check.expect(createdPair && exactA && exactB,
        "independent AVIO contexts read on separate threads");
    std::cout << "cases=" << check.cases << " failures=" << check.failures << '\n';
    return check.failures ? 1 : 0;
}
