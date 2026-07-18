#include "../src/Probe/FrameCountPolicy.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using AveMediaBridge::Probe::FrameCountPolicyState;
using AveMediaBridge::Probe::OggVorbisTerminalScan;

constexpr std::int64_t kExactFrames = 105840000;
constexpr std::int64_t kMismatchedFrames = 105839872;

struct TestFailure {
    std::string name;
    std::string message;
};

FrameCountPolicyState makeEstimatedState(
    const std::string& formatName,
    AVCodecID codecId,
    const std::string& codecName) {
    FrameCountPolicyState state;
    state.formatName = formatName;
    state.selectedAudio.codecId = codecId;
    state.selectedAudio.codecName = codecName;
    state.selectedAudio.sampleRate = 44100;
    state.selectedAudio.channels = 2;
    state.streamDurationFrames = kExactFrames;
    state.formatDurationFrames = kExactFrames;
    state.decodedSampleFrames = kExactFrames;
    state.decodedSampleFramesKind = "estimated";
    state.decodedSampleFramesTrust = "unknown";
    state.decodedSampleFramesSource = "stream_duration_estimate";
    state.frameCountPolicyReason = "duration-derived compressed frame count is estimated";
    return state;
}

OggVorbisTerminalScan makeTerminalScan(std::int64_t eosGranule) {
    OggVorbisTerminalScan scan;
    scan.attempted = true;
    scan.scanComplete = true;
    scan.vorbisSerialKnown = true;
    scan.vorbisSerial = 1234;
    scan.pageCount = 8;
    scan.bosPageCount = 1;
    scan.eosPageCount = 1;
    scan.serialNumberCount = 1;
    scan.vorbisBosCount = 1;
    scan.vorbisEosCount = 1;
    scan.eosObserved = eosGranule >= 0;
    scan.selectedAudioEosGranule = eosGranule;
    return scan;
}

void decide(FrameCountPolicyState& state, const OggVorbisTerminalScan& scan) {
    AveMediaBridge::Probe::recordOggVorbisTerminalScan(state, scan);
    AveMediaBridge::Probe::applyOggVorbisExactExtentPolicy(state);
    AveMediaBridge::Probe::finalizeFrameCountTrustPolicy(
        state,
        true,
        state.selectedAudio.codecId == AV_CODEC_ID_PCM_F32LE);
}

bool expectExact(
    std::vector<TestFailure>& failures,
    const std::string& name,
    const FrameCountPolicyState& state) {
    if (state.decodedSampleFrames == kExactFrames &&
        state.decodedSampleFramesKind == "exact" &&
        state.decodedSampleFramesTrust == "authoritative" &&
        state.decodedSampleFramesSource == "ogg_vorbis_eos_granule" &&
        state.frameCountPolicyReason == "ogg_vorbis_exact_extent_proven") {
        return true;
    }
    failures.push_back({
        name,
        "expected exact authoritative Ogg/Vorbis decision, got frames=" +
            std::to_string(state.decodedSampleFrames) +
            " kind=" + state.decodedSampleFramesKind +
            " trust=" + state.decodedSampleFramesTrust +
            " source=" + state.decodedSampleFramesSource +
            " reason=" + state.frameCountPolicyReason});
    return false;
}

bool expectUnsafe(
    std::vector<TestFailure>& failures,
    const std::string& name,
    const FrameCountPolicyState& state) {
    if (state.decodedSampleFrames == kExactFrames &&
        state.decodedSampleFramesKind == "estimated" &&
        state.decodedSampleFramesTrust == "unsafe_estimated" &&
        state.decodedSampleFramesSource == "stream_duration_estimate") {
        return true;
    }
    failures.push_back({
        name,
        "expected unsafe provisional decision, got frames=" +
            std::to_string(state.decodedSampleFrames) +
            " kind=" + state.decodedSampleFramesKind +
            " trust=" + state.decodedSampleFramesTrust +
            " source=" + state.decodedSampleFramesSource +
            " reason=" + state.frameCountPolicyReason});
    return false;
}

void runTests(std::vector<TestFailure>& failures) {
    {
        auto state = makeEstimatedState("ogg", AV_CODEC_ID_VORBIS, "vorbis");
        decide(state, makeTerminalScan(kExactFrames));
        expectExact(failures, "exact Ogg/Vorbis", state);
    }
    {
        auto state = makeEstimatedState("ogg", AV_CODEC_ID_VORBIS, "vorbis");
        decide(state, makeTerminalScan(kMismatchedFrames));
        expectUnsafe(failures, "EOS mismatch", state);
    }
    {
        auto state = makeEstimatedState("ogg", AV_CODEC_ID_VORBIS, "vorbis");
        state.packetPtsSpanFrames = kMismatchedFrames;
        decide(state, makeTerminalScan(kExactFrames));
        expectUnsafe(failures, "packet terminal mismatch", state);
    }
    {
        auto state = makeEstimatedState("ogg", AV_CODEC_ID_VORBIS, "vorbis");
        decide(state, makeTerminalScan(-1));
        expectUnsafe(failures, "missing EOS", state);
    }
    {
        auto state = makeEstimatedState("ogg", AV_CODEC_ID_VORBIS, "vorbis");
        auto scan = makeTerminalScan(kExactFrames);
        scan.scanComplete = false;
        scan.truncated = true;
        decide(state, scan);
        expectUnsafe(failures, "truncated scan", state);
    }
    {
        auto state = makeEstimatedState("ogg", AV_CODEC_ID_VORBIS, "vorbis");
        auto scan = makeTerminalScan(kExactFrames);
        scan.vorbisBosCount = 2;
        scan.vorbisEosCount = 2;
        scan.chainedOrAmbiguous = true;
        decide(state, scan);
        expectUnsafe(failures, "chained Ogg unsupported", state);
    }
    {
        auto state = makeEstimatedState("ogg", AV_CODEC_ID_VORBIS, "vorbis");
        state.skipSamplesStart = 128;
        state.skipSamplesTotal = 128;
        decide(state, makeTerminalScan(kExactFrames));
        expectUnsafe(failures, "skip/discard conflict", state);
    }
    {
        auto state = makeEstimatedState("ogg", AV_CODEC_ID_VORBIS, "vorbis");
        decide(state, makeTerminalScan(kExactFrames));
        expectExact(failures, "exact OGA Vorbis", state);
    }
    {
        auto state = makeEstimatedState("ogg", AV_CODEC_ID_VORBIS, "vorbis");
        auto scan = makeTerminalScan(kExactFrames);
        scan.serialNumberCount = 2;
        scan.bosPageCount = 2;
        scan.eosPageCount = 2;
        decide(state, scan);
        expectExact(failures, "OGV with selected-audio terminal proof", state);
    }
    {
        auto state = makeEstimatedState("ogg", AV_CODEC_ID_VORBIS, "vorbis");
        auto scan = makeTerminalScan(kExactFrames);
        scan.vorbisSerialKnown = false;
        scan.vorbisBosCount = 0;
        scan.vorbisEosCount = 0;
        scan.eosObserved = false;
        scan.selectedAudioEosGranule = -1;
        decide(state, scan);
        expectUnsafe(failures, "OGV without selected-audio proof", state);
    }
    {
        auto state = makeEstimatedState("matroska,webm", AV_CODEC_ID_VORBIS, "vorbis");
        decide(state, makeTerminalScan(kExactFrames));
        expectUnsafe(failures, "Matroska Vorbis unchanged", state);
    }
    {
        auto state = makeEstimatedState("matroska,webm", AV_CODEC_ID_VORBIS, "vorbis");
        decide(state, makeTerminalScan(kExactFrames));
        expectUnsafe(failures, "WebM Vorbis unchanged", state);
    }
    {
        auto state = makeEstimatedState("ogg", AV_CODEC_ID_OPUS, "opus");
        decide(state, makeTerminalScan(kExactFrames));
        expectUnsafe(failures, "Ogg Opus unchanged by Vorbis policy", state);
    }
    {
        auto state = makeEstimatedState("flac", AV_CODEC_ID_FLAC, "flac");
        decide(state, makeTerminalScan(kExactFrames));
        expectUnsafe(failures, "FLAC unchanged", state);
    }
    {
        auto state = makeEstimatedState("mov,mp4,m4a,3gp,3g2,mj2", AV_CODEC_ID_AAC, "aac");
        decide(state, makeTerminalScan(kExactFrames));
        expectUnsafe(failures, "M4A AAC unchanged", state);
    }
}

}  // namespace

int main() {
    std::vector<TestFailure> failures;
    runTests(failures);
    for (const TestFailure& failure : failures) {
        std::cerr << failure.name << ": " << failure.message << '\n';
    }
    if (!failures.empty()) {
        return EXIT_FAILURE;
    }
    std::cout << "FrameCountPolicy Ogg/Vorbis exact authority tests passed\n";
    return EXIT_SUCCESS;
}
