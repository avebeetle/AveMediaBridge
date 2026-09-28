#include "Input/MediaInputSource.hpp"
#include "Input/ReaderInputError.hpp"
#include "Input/DemuxSession.hpp"
#include "Probe/MediaProbeService.hpp"
#include "Probe/PacketScan.hpp"

#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/log.h>
}

using namespace AveMediaBridge;

namespace {
struct Checks {
    int count = 0;
    int failures = 0;
    void expect(bool condition, const std::string& label) {
        ++count;
        if (!condition) {
            ++failures;
            std::cerr << "FAIL " << label << '\n';
        }
    }
};

class ExpectedFfmpegLog final {
public:
    ExpectedFfmpegLog() {
        active_ = this;
        av_log_set_callback(&ExpectedFfmpegLog::capture);
    }
    ~ExpectedFfmpegLog() {
        av_log_set_callback(av_log_default_callback);
        active_ = nullptr;
    }
    ExpectedFfmpegLog(const ExpectedFfmpegLog&) = delete;
    ExpectedFfmpegLog& operator=(const ExpectedFfmpegLog&) = delete;

    void expectOnly(Checks& check, const std::string& label,
        std::initializer_list<std::string_view> allowed) const {
        std::istringstream lines(text_);
        std::string line;
        bool unexpected = overflow_;
        while (std::getline(lines, line)) {
            if (line.empty()) continue;
            bool accepted = false;
            for (const auto expected : allowed) {
                if (line.find(expected) != std::string::npos) {
                    accepted = true;
                    break;
                }
            }
            if (!accepted) {
                unexpected = true;
                std::cerr << "Unexpected FFmpeg diagnostic in " << label
                    << ": " << line << '\n';
            }
        }
        check.expect(!unexpected, label + " FFmpeg diagnostics are expected only");
    }

private:
    static void capture(void*, int level, const char* format, va_list args) {
        if (!active_ || level > av_log_get_level()) return;
        char buffer[1024]{};
        const int length = std::vsnprintf(buffer, sizeof buffer, format, args);
        if (length < 0 || length >= static_cast<int>(sizeof buffer)) {
            active_->overflow_ = true;
            return;
        }
        try {
            active_->text_.append(buffer, static_cast<std::size_t>(length));
        } catch (...) {
            active_->overflow_ = true;
        }
    }
    static ExpectedFfmpegLog* active_;
    std::string text_;
    bool overflow_ = false;
};

ExpectedFfmpegLog* ExpectedFfmpegLog::active_ = nullptr;

struct Bytes {
    std::vector<std::uint8_t> data;
    int reads = 0;
    int checks = 0;
    int failAtRead = -1;
    int cancelAtCheck = -1;
    int cancelAfterReads = -1;
};

AMBI_Status __cdecl readAt(void* user, std::uint64_t offset, void* destination,
    std::uint32_t requested, std::uint32_t* count) {
    auto& bytes = *static_cast<Bytes*>(user);
    *count = 0;
    if (bytes.failAtRead >= 0 && bytes.reads >= bytes.failAtRead)
        return AMBI_IO_ERROR;
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
    return ((bytes.cancelAtCheck >= 0 && bytes.checks >= bytes.cancelAtCheck) ||
        (bytes.cancelAfterReads >= 0 && bytes.reads > bytes.cancelAfterReads))
        ? AMBI_CANCELED : AMBI_OK;
}

Input::MediaInputSource sourceFor(Bytes& bytes,
    std::string label = "Z:/nonexistent/reader-label.mp4") {
    AMBI_SourceV1 descriptor{};
    descriptor.structSize = sizeof descriptor;
    descriptor.abiVersion = AMBI_ABI_VERSION;
    descriptor.byteSize = bytes.data.size();
    descriptor.sourceToken[0] = 0x74;
    descriptor.sourceToken[15] = 0xA2;
    descriptor.user = &bytes;
    descriptor.readAt = &readAt;
    descriptor.checkCancel = &checkCancel;
    return Input::MediaInputSource::fromStable(descriptor, std::move(label));
}

Bytes load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), {})};
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

void compareCompleteJson(Checks& check, Probe::FastProbeResult reader,
    const Probe::FastProbeResult& legacy, const std::string& label) {
    reader.document.sourcePath = legacy.document.sourcePath;
    check.expect(reader.document.probeScore == 0 &&
        legacy.document.probeScore > 0,
        label + " forced MOV has no autodetection score");
    // A forced demuxer has no autodetection score. Compare every other JSON
    // field while the separate assertion above keeps that difference explicit.
    reader.document.probeScore = legacy.document.probeScore;
    const char* dataRoot = std::getenv("AVEVOICE_DATA_ROOT");
    if (!dataRoot || !std::filesystem::is_directory(dataRoot)) {
        check.expect(false, label + " isolated AVEVOICE_DATA_ROOT exists");
        return;
    }
    const auto scratch = std::filesystem::path(dataRoot) /
        ("task2-probe-parity-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code fileError;
    if (!std::filesystem::create_directory(scratch, fileError) || fileError) {
        check.expect(false, label + " isolated probe scratch directory created");
        return;
    }
    const auto readerPath = scratch / "reader.json";
    const auto legacyPath = scratch / "legacy.json";
    std::string error;
    const bool written = Probe::writeFastProbeJson(readerPath, reader, error) &&
        Probe::writeFastProbeJson(legacyPath, legacy, error);
    check.expect(written, label + " probe JSON written");
    if (written) {
        const std::string readerJson = readText(readerPath);
        const std::string legacyJson = readText(legacyPath);
        if (readerJson != legacyJson) {
            std::istringstream readerLines(readerJson);
            std::istringstream legacyLines(legacyJson);
            std::string a, b;
            while (std::getline(readerLines, a) && std::getline(legacyLines, b)) {
                if (a != b) {
                    std::cerr << label << " JSON first difference: reader=" << a
                        << " path=" << b << '\n';
                    break;
                }
            }
        }
        check.expect(readerJson == legacyJson, label + " complete probe JSON parity");
    }
    std::filesystem::remove(readerPath, fileError);
    std::filesystem::remove(legacyPath, fileError);
    std::filesystem::remove(scratch, fileError);
}

void compareProbe(Checks& check, const Probe::FastProbeResult& reader,
    const Probe::FastProbeResult& legacy, const std::string& label) {
    const auto& r = reader.document;
    const auto& p = legacy.document;
    check.expect(r.bestAudioStreamIndex == p.bestAudioStreamIndex,
        label + " selected stream");
    check.expect(r.selectedAudio.codecId == p.selectedAudio.codecId &&
        r.selectedAudio.sampleRate == p.selectedAudio.sampleRate &&
        r.selectedAudio.channels == p.selectedAudio.channels &&
        r.selectedAudio.timeBase == p.selectedAudio.timeBase &&
        r.channelLayout == p.channelLayout, label + " codec/rate/layout/time base");
    check.expect(r.decodedSampleFrames == p.decodedSampleFrames,
        label + " frames");
    check.expect(r.decodedSampleFramesTrust == p.decodedSampleFramesTrust,
        label + " trust");
    check.expect(r.decodedSampleFramesSource == p.decodedSampleFramesSource &&
        r.decodedSampleFramesKind == p.decodedSampleFramesKind &&
        r.decodedSampleFramesBeforeCorrection == p.decodedSampleFramesBeforeCorrection &&
        r.frameCountPolicyReason == p.frameCountPolicyReason,
        label + " authority");
    check.expect(r.packetPtsSpanFrames == p.packetPtsSpanFrames &&
        r.packetDurationSumFrames == p.packetDurationSumFrames &&
        r.packetFrameCountCandidateUsed == p.packetFrameCountCandidateUsed &&
        r.skipSamplesStart == p.skipSamplesStart &&
        r.skipSamplesEnd == p.skipSamplesEnd &&
        r.skipSamplesTotal == p.skipSamplesTotal,
        label + " packet/skip evidence");
    check.expect(reader.totalPresentation.frames == legacy.totalPresentation.frames &&
        reader.totalPresentation.trust == legacy.totalPresentation.trust &&
        reader.totalPresentation.source == legacy.totalPresentation.source &&
        reader.totalPresentation.validation == legacy.totalPresentation.validation,
        label + " total presentation");
    check.expect(r.mp3HeaderPresentationStatus == "not_eligible" &&
        r.mp4Mp3SampleTableStatus == "not_eligible" &&
        r.oggOpusSequentialStatus == "not_eligible" &&
        r.matroskaAacSequentialStatus == "not_eligible" &&
        r.adtsAacSequentialStatus == "not_eligible" &&
        r.dolbySequentialStatus == "not_eligible",
        label + " direct parser ineligible");
    check.expect(reader.stableBinding.has_value(), label + " binding populated");
    check.expect(r.packetPtsSpanFrames == 0 && r.packetDurationSumFrames == 0,
        label + " admitted preflight does not packet scan");
}

void compareTotalPresentation(Checks& check,
    const Probe::FastProbeResult& reader,
    const Probe::FastProbeResult& legacy,
    const std::string& label) {
    const auto& a = reader.totalPresentation;
    const auto& b = legacy.totalPresentation;
    check.expect(a.frames == b.frames && a.trust == b.trust &&
        a.source == b.source && a.domain == b.domain &&
        a.sampleRate == b.sampleRate && a.exactRescale == b.exactRescale &&
        a.conflict == b.conflict && a.validation == b.validation,
        label + " complete total-presentation evidence");
}

void compareScans(Checks& check, const Input::MediaInputSource& source,
    const Input::SelectedAudioBinding& binding, const std::string& path,
    const std::string& label) {
    const Probe::PacketScanOptions options{4 * 1024 * 1024, 3 * AV_TIME_BASE};
    const auto pathScan = Probe::scanAudioPresentationEvidence(path,
        binding.streamIndex, binding.sampleRate,
        static_cast<AVCodecID>(binding.codecId), options);
    const auto stableScan = Probe::scanAudioPresentationEvidence(source, binding, options);
    check.expect(stableScan.packetTiming.reachedEof && stableScan.gapless.reachedEof,
        label + " full stable scan reaches EOF");
    check.expect(stableScan.packetTiming.audioPacketCount ==
        pathScan.packetTiming.audioPacketCount &&
        stableScan.packetTiming.packetDurationSumFrames ==
        pathScan.packetTiming.packetDurationSumFrames &&
        stableScan.packetTiming.sampleExactPacketDurationSumFrames ==
        pathScan.packetTiming.sampleExactPacketDurationSumFrames &&
        stableScan.packetTiming.packetPtsSpanFrames ==
        pathScan.packetTiming.packetPtsSpanFrames &&
        stableScan.packetTiming.codecFrameCountFrames ==
        pathScan.packetTiming.codecFrameCountFrames,
        label + " timing scan parity");
    check.expect(stableScan.gapless.skipSamplesStart == pathScan.gapless.skipSamplesStart &&
        stableScan.gapless.skipSamplesEnd == pathScan.gapless.skipSamplesEnd &&
        stableScan.gapless.audioPacketsScanned == pathScan.gapless.audioPacketsScanned,
        label + " gapless scan parity");
    const auto packetOnly = Probe::scanPacketFrameCountCandidates(source, binding, options);
    const auto gaplessOnly = Probe::scanGaplessSkipSampleSideData(source, binding, options);
    check.expect(packetOnly.reachedEof &&
        packetOnly.audioPacketCount == stableScan.packetTiming.audioPacketCount &&
        packetOnly.packetDurationSumFrames ==
            stableScan.packetTiming.packetDurationSumFrames,
        label + " packet-only stable scan");
    check.expect(gaplessOnly.reachedEof &&
        gaplessOnly.audioPacketsScanned == stableScan.gapless.audioPacketsScanned &&
        gaplessOnly.skipSamplesEnd == stableScan.gapless.skipSamplesEnd,
        label + " gapless-only stable scan");
    auto wrong = binding;
    wrong.trackId += 1;
    bool rejected = false;
    try {
        (void)Probe::scanPacketFrameCountCandidates(source, wrong, options);
    } catch (const Input::ReaderInputError& error) {
        rejected = error.failure() == Input::ReaderInputFailure::BindingMismatch;
    }
    check.expect(rejected, label + " changed track rejects scan");
    wrong = binding;
    wrong.streamIndex = -1;
    rejected = false;
    try {
        (void)Probe::scanGaplessSkipSampleSideData(source, wrong, options);
    } catch (const Input::ReaderInputError& error) {
        rejected = error.failure() == Input::ReaderInputFailure::BindingMismatch;
    }
    check.expect(rejected, label + " invalid selected index rejects scan");
}
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    Checks check;
    const std::filesystem::path generated(argv[1]);
    const std::filesystem::path lab(argv[2]);
    const std::vector<std::filesystem::path> admitted{
        generated / "reader_stereo_front_aac.mp4",
        generated / "reader_stereo_tail_aac.mp4",
        generated / "reader_two_aac_default_second.mp4",
        lab / "exact_authority/valid/aac/EA035_m4a_aac_96000_10241.m4a",
        lab / "exact_authority/valid/aac/EA127_mp4_aac_88200_88199.mp4",
        lab / "exact_authority/valid/aac/EA098_m4a_aac_48000_1.m4a",
        lab / "av_sync/valid/mp4/AV018_nonzero_audio_media_time_mp4.mp4",
        lab / "av_sync/valid/mov/AV019_empty_edit_before_audio_mov.mov"};
    for (const auto& path : admitted) {
        const std::string label = path.filename().string();
        Bytes bytes = load(path);
        if (bytes.data.empty()) {
            std::cerr << "missing fixture " << path << '\n';
            return 2;
        }
        const auto source = sourceFor(bytes);
        const auto legacy = Probe::runFastProbe(path.string());
        const auto reader = Probe::runFastProbe(source);
        compareProbe(check, reader, legacy, label);
        compareCompleteJson(check, reader, legacy, label);
        check.expect(bytes.reads > 0 && bytes.checks > 0,
            label + " memory callback used with nonexistent display label");
        if (reader.stableBinding) {
            check.expect(reader.stableBinding->byteSize == bytes.data.size() &&
                reader.stableBinding->streamIndex == legacy.document.bestAudioStreamIndex,
                label + " receipt source size and selected stream");
            compareScans(check, source, *reader.stableBinding, path.string(), label);
        }
    }
    {
        const auto path = generated / "reader_stereo_front_aac.mp4";
        Bytes bytes = load(path);
        const auto legacy = Probe::runFastProbeWithTestPadding(
            Input::MediaInputSource::fromPath(path.string()));
        const auto stable = Probe::runFastProbeWithTestPadding(sourceFor(bytes));
        check.expect(legacy.document.packetDurationSumFrames > 0 &&
            legacy.document.gaplessAudioPacketsScanned > 0,
            "forced metadata enters legacy conditional packet and gapless scan");
        check.expect(stable.document.packetDurationSumFrames ==
            legacy.document.packetDurationSumFrames &&
            stable.document.gaplessAudioPacketsScanned ==
            legacy.document.gaplessAudioPacketsScanned &&
            stable.document.skipSamplesStart == legacy.document.skipSamplesStart,
            "forced metadata routes stable conditional scans with parity");
        compareCompleteJson(check, stable, legacy, "forced padding stable");
        compareTotalPresentation(check, stable, legacy, "forced padding stable");
        Bytes noLabel = load(path);
        const auto emptyLabel = Probe::runFastProbeWithTestPadding(
            sourceFor(noLabel, ""));
        check.expect(emptyLabel.document.packetDurationSumFrames ==
            legacy.document.packetDurationSumFrames &&
            emptyLabel.document.gaplessAudioPacketsScanned ==
            legacy.document.gaplessAudioPacketsScanned,
            "empty display label cannot suppress conditional scan");
        compareCompleteJson(check, emptyLabel, legacy, "forced padding empty label");
        compareTotalPresentation(check, emptyLabel, legacy,
            "forced padding empty label");
    }
    for (const auto& path : {
        generated / "reader_mp4_mp3_control.mp4",
        generated / "reader_mp4_alac_control.m4a",
        generated / "reader_mp4_no_audio.mp4"}) {
        Bytes bytes = load(path);
        if (bytes.data.empty()) return 2;
        bool unsupported = false;
        try {
            (void)Probe::runFastProbe(sourceFor(bytes));
        } catch (const Input::ReaderInputError& error) {
            unsupported = error.failure() == Input::ReaderInputFailure::Unsupported;
        }
        check.expect(unsupported, path.filename().string() + " stable route rejects unsupported media");
        const auto legacy = Probe::runFastProbe(path.string());
        check.expect(legacy.streamInfoFound, path.filename().string() + " legacy route unchanged");
    }
    {
        const auto path = lab / "exact_authority/valid/pcm_lossless/EA002_wav_s16_16000_257.wav";
        Bytes bytes = load(path);
        if (bytes.data.empty()) return 2;
        bool unsupported = false;
        ExpectedFfmpegLog expectedLog;
        try {
            (void)Probe::runFastProbe(sourceFor(bytes));
        } catch (const Input::ReaderInputError& error) {
            unsupported = error.failure() == Input::ReaderInputFailure::Unsupported;
        }
        check.expect(unsupported, "stable route classifies non-MOV WAV as unsupported");
        expectedLog.expectOnly(check, "non-MOV WAV rejection",
            {"moov atom not found"});
    }
    {
        const auto path = generated / "reader_demux_aac.m4a";
        Bytes clean = load(path);
        if (clean.data.empty()) return 2;
        const auto binding = Probe::runFastProbe(sourceFor(clean)).stableBinding;
        check.expect(binding.has_value(), "large fixture gives selected receipt");
        if (binding) {
            {
                Bytes failed = load(path);
                failed.failAtRead = 1;
                bool inputFailed = false;
                ExpectedFfmpegLog faultLog;
                try {
                    (void)Probe::scanAudioPresentationEvidence(sourceFor(failed), *binding,
                        {4 * 1024 * 1024, 3 * AV_TIME_BASE});
                } catch (const Input::ReaderInputError& error) {
                    inputFailed = error.failure() == Input::ReaderInputFailure::InputFailed;
                }
                check.expect(inputFailed && failed.reads == 1,
                    "stable scan callback failure remains fatal after buffered prefix");
                faultLog.expectOnly(check, "callback fault",
                    {"Packet corrupt", "moov atom not found"});
            }
            Bytes discovery = load(path);
            std::unique_ptr<Input::DemuxSession> discoverySession;
            const int discoveryOpen = Input::DemuxSession::open(sourceFor(discovery),
                {true, 4 * 1024 * 1024, 3 * AV_TIME_BASE}, discoverySession);
            const int discoveryInfo = discoveryOpen == 0 && discoverySession
                ? avformat_find_stream_info(discoverySession->get(), nullptr) : -1;
            check.expect(discoveryInfo == 0 && discoverySession &&
                discoverySession->inputError() == 0 && discovery.reads > 0,
                "phase calibration reaches stable stream discovery");
            {
                Bytes canceled = load(path);
                canceled.cancelAfterReads = discovery.reads;
                bool stopped = false;
                ExpectedFfmpegLog cancelLog;
                try {
                    (void)Probe::scanAudioPresentationEvidence(sourceFor(canceled), *binding,
                        {4 * 1024 * 1024, 3 * AV_TIME_BASE});
                } catch (const Input::ReaderInputError& error) {
                    stopped = error.failure() == Input::ReaderInputFailure::Canceled;
                }
                check.expect(stopped && canceled.reads > discovery.reads,
                    "stable scan cancellation after stream discovery remains fatal");
                cancelLog.expectOnly(check, "late cancellation",
                    {"Packet corrupt"});
            }
        }
    }
    std::cout << "checks=" << check.count << " failures=" << check.failures << '\n';
    return check.failures ? 1 : 0;
}
