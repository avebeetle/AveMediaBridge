#include "../src/Ffmpeg/FfmpegDeleters.hpp"
#include "../src/Probe/FrameCountPolicy.hpp"
#include "../src/Probe/PacketScan.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace Probe = AveMediaBridge::Probe;
namespace Ffmpeg = AveMediaBridge::Ffmpeg;

constexpr Probe::PacketScanOptions kScanOptions{
    4LL * 1024LL * 1024LL,
    3LL * AV_TIME_BASE
};

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void writeLittleEndian16(std::ofstream& output, std::uint16_t value) {
    const std::array<char, 2> bytes{
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff)
    };
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void writeLittleEndian32(std::ofstream& output, std::uint32_t value) {
    const std::array<char, 4> bytes{
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
        static_cast<char>((value >> 16) & 0xff),
        static_cast<char>((value >> 24) & 0xff)
    };
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::filesystem::path writeDeterministicWaveFixture() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "avemediabridge_presentation_scan_fixture.wav";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    expect(static_cast<bool>(output), "failed to create deterministic WAV fixture");

    constexpr std::uint32_t sampleRate = 8000;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bitsPerSample = 16;
    constexpr std::uint32_t sampleCount = 32;
    constexpr std::uint32_t dataBytes = sampleCount * channels * (bitsPerSample / 8);

    output.write("RIFF", 4);
    writeLittleEndian32(output, 36 + dataBytes);
    output.write("WAVE", 4);
    output.write("fmt ", 4);
    writeLittleEndian32(output, 16);
    writeLittleEndian16(output, 1);
    writeLittleEndian16(output, channels);
    writeLittleEndian32(output, sampleRate);
    writeLittleEndian32(output, sampleRate * channels * (bitsPerSample / 8));
    writeLittleEndian16(output, channels * (bitsPerSample / 8));
    writeLittleEndian16(output, bitsPerSample);
    output.write("data", 4);
    writeLittleEndian32(output, dataBytes);
    for (std::uint32_t i = 0; i < sampleCount; ++i) {
        writeLittleEndian16(output, static_cast<std::uint16_t>(i * 31));
    }
    output.close();
    expect(static_cast<bool>(output), "failed to finish deterministic WAV fixture");
    return path;
}

std::array<std::uint8_t, 10> skipSideData(std::uint32_t start, std::uint32_t end) {
    return {
        static_cast<std::uint8_t>(start & 0xff),
        static_cast<std::uint8_t>((start >> 8) & 0xff),
        static_cast<std::uint8_t>((start >> 16) & 0xff),
        static_cast<std::uint8_t>((start >> 24) & 0xff),
        static_cast<std::uint8_t>(end & 0xff),
        static_cast<std::uint8_t>((end >> 8) & 0xff),
        static_cast<std::uint8_t>((end >> 16) & 0xff),
        static_cast<std::uint8_t>((end >> 24) & 0xff),
        0,
        0
    };
}

bool packetScansEqual(
    const Probe::PacketFrameCountScan& left,
    const Probe::PacketFrameCountScan& right) {
    return left.packetCount == right.packetCount &&
        left.audioPacketCount == right.audioPacketCount &&
        left.packetsWithDuration == right.packetsWithDuration &&
        left.packetsWithTimestamp == right.packetsWithTimestamp &&
        left.firstPacketPts == right.firstPacketPts &&
        left.lastPacketPts == right.lastPacketPts &&
        left.lastPacketEndPts == right.lastPacketEndPts &&
        left.lastPacketDuration == right.lastPacketDuration &&
        left.packetPtsSpanFrames == right.packetPtsSpanFrames &&
        left.packetPtsSpanWithoutLastDurationFrames == right.packetPtsSpanWithoutLastDurationFrames &&
        left.packetPtsSpanPlusLastDurationFrames == right.packetPtsSpanPlusLastDurationFrames &&
        left.packetDurationSumFrames == right.packetDurationSumFrames &&
        left.packetDurationArithmeticValid == right.packetDurationArithmeticValid &&
        left.sampleExactPacketDurationSumFrames == right.sampleExactPacketDurationSumFrames &&
        left.packetsWithSampleExactDuration == right.packetsWithSampleExactDuration &&
        left.codecFrameCountFrames == right.codecFrameCountFrames &&
        left.packetsWithCodecFrameCount == right.packetsWithCodecFrameCount &&
        left.aacFrameCountCandidateFrames == right.aacFrameCountCandidateFrames &&
        left.mp3FrameCountCandidateFrames == right.mp3FrameCountCandidateFrames &&
        left.mp2FrameCountCandidateFrames == right.mp2FrameCountCandidateFrames &&
        left.wmav2FrameCountCandidateFrames == right.wmav2FrameCountCandidateFrames &&
        std::abs(left.averagePacketDurationFrames - right.averagePacketDurationFrames) <= 1e-12 &&
        left.packetPtsMonotonic == right.packetPtsMonotonic &&
        left.codecFrameCountKnown == right.codecFrameCountKnown &&
        left.codecFrameCountExact == right.codecFrameCountExact &&
        left.reachedEof == right.reachedEof &&
        left.readError == right.readError &&
        left.warning == right.warning;
}

bool gaplessScansEqual(
    const Probe::GaplessSkipSampleScan& left,
    const Probe::GaplessSkipSampleScan& right) {
    return left.packetSkipSamplesStart == right.packetSkipSamplesStart &&
        left.packetSkipSamplesEnd == right.packetSkipSamplesEnd &&
        left.streamInitialPadding == right.streamInitialPadding &&
        left.streamTrailingPadding == right.streamTrailingPadding &&
        left.skipSamplesStart == right.skipSamplesStart &&
        left.skipSamplesEnd == right.skipSamplesEnd &&
        left.skipSamplesTotal == right.skipSamplesTotal &&
        left.sideDataPacketCount == right.sideDataPacketCount &&
        left.audioPacketsScanned == right.audioPacketsScanned &&
        left.reachedEof == right.reachedEof &&
        left.readError == right.readError &&
        left.source == right.source &&
        left.warning == right.warning;
}

void observeSelected(
    Probe::PacketFrameCountAccumulator& accumulator,
    std::int64_t pts,
    std::int64_t duration,
    int codecFrameSamples = 0) {
    accumulator.observe(Probe::PacketFrameCountObservation{
        true,
        pts,
        AV_NOPTS_VALUE,
        duration,
        codecFrameSamples
    });
}

void testPacketTimingAccumulator() {
    Probe::PacketFrameCountAccumulator accumulator(48000, AV_CODEC_ID_AAC, AVRational{1, 48000});
    observeSelected(accumulator, 0, 100, 1024);
    accumulator.observe(Probe::PacketFrameCountObservation{false, 50, 50, 50});
    observeSelected(accumulator, 100, 200, 1024);
    observeSelected(accumulator, 300, 300, 1024);
    const Probe::PacketFrameCountScan result = accumulator.finalize();

    expect(result.packetCount == 4, "packet count accumulation changed");
    expect(result.audioPacketCount == 3, "audio packet count accumulation changed");
    expect(result.packetsWithDuration == 3, "duration packet count changed");
    expect(result.packetDurationSumFrames == 600, "packet duration sum changed");
    expect(result.packetDurationArithmeticValid, "valid packet duration arithmetic was rejected");
    expect(result.averagePacketDurationFrames == 200.0, "packet duration average changed");
    expect(result.firstPacketPts == 0, "first packet PTS changed");
    expect(result.lastPacketPts == 300, "last packet PTS changed");
    expect(result.lastPacketEndPts == 600, "last packet end changed");
    expect(result.packetPtsMonotonic, "monotonic PTS sequence rejected");
    expect(result.aacFrameCountCandidateFrames == 3 * 1024, "AAC candidate changed");
    expect(result.codecFrameCountKnown, "codec frame count was not accumulated");
    expect(result.codecFrameCountFrames == 3 * 1024, "codec frame sample sum changed");
}

void testNonMonotonicPts() {
    Probe::PacketFrameCountAccumulator accumulator(44100, AV_CODEC_ID_NONE, AVRational{1, 44100});
    observeSelected(accumulator, 100, 10);
    observeSelected(accumulator, 90, 10);
    const Probe::PacketFrameCountScan result = accumulator.finalize();
    expect(!result.packetPtsMonotonic, "non-monotonic PTS sequence accepted");
    expect(result.firstPacketPts == 90, "minimum first PTS changed");
    expect(result.lastPacketPts == 100, "maximum last PTS changed");
}

void testWrappedPacketDurationIsDiagnosticOnly() {
    Probe::PacketFrameCountAccumulator accumulator(
        48000, AV_CODEC_ID_VORBIS, AVRational{1, 48000});
    observeSelected(accumulator, 0, 128);
    observeSelected(accumulator, 128, 4294967231LL);
    const Probe::PacketFrameCountScan result = accumulator.finalize();

    expect(
        result.packetDurationSumFrames == 4294967359LL,
        "wrapped packet duration diagnostic sum changed");
    expect(
        !result.packetDurationArithmeticValid,
        "wrapped packet duration was promoted as validation authority");
}

void testCodecCandidates() {
    Probe::PacketFrameCountAccumulator mp3(44100, AV_CODEC_ID_MP3, AVRational{1, 44100});
    Probe::PacketFrameCountAccumulator mp2(44100, AV_CODEC_ID_MP2, AVRational{1, 44100});
    Probe::PacketFrameCountAccumulator wmav2(44100, AV_CODEC_ID_WMAV2, AVRational{1, 44100});
    observeSelected(mp3, 0, 1);
    observeSelected(mp2, 0, 1);
    observeSelected(wmav2, 0, 1);
    expect(mp3.finalize().mp3FrameCountCandidateFrames == 1152, "MP3 candidate changed");
    expect(mp2.finalize().mp2FrameCountCandidateFrames == 1152, "MP2 candidate changed");
    expect(wmav2.finalize().wmav2FrameCountCandidateFrames == 2048, "WMAV2 candidate changed");
}

void testGaplessAccumulator() {
    const auto sideData = skipSideData(100, 50);
    Probe::GaplessSkipAccumulator packetOnly(0, 0);
    packetOnly.observe(Probe::GaplessSkipObservation{true, sideData.data(), sideData.size()});
    const Probe::GaplessSkipSampleScan packetResult = packetOnly.finalize();
    expect(packetResult.skipSamplesStart == 100, "packet start skip changed");
    expect(packetResult.skipSamplesEnd == 50, "packet end discard changed");
    expect(packetResult.skipSamplesTotal == 150, "packet skip total changed");
    expect(packetResult.source == "packet_side_data_skip_samples", "packet skip source changed");

    Probe::GaplessSkipAccumulator streamOnly(120, 80);
    streamOnly.observe(Probe::GaplessSkipObservation{true, nullptr, 0});
    const Probe::GaplessSkipSampleScan streamResult = streamOnly.finalize();
    expect(streamResult.skipSamplesStart == 120, "stream initial padding changed");
    expect(streamResult.skipSamplesEnd == 80, "stream trailing padding changed");
    expect(streamResult.source == "stream_padding", "stream padding source changed");

    Probe::GaplessSkipAccumulator combined(120, 40);
    combined.observe(Probe::GaplessSkipObservation{true, sideData.data(), sideData.size()});
    const Probe::GaplessSkipSampleScan combinedResult = combined.finalize();
    expect(combinedResult.skipSamplesStart == 120, "gapless max start semantics changed");
    expect(combinedResult.skipSamplesEnd == 50, "gapless max end semantics changed");
    expect(
        combinedResult.source == "packet_side_data_skip_samples_and_stream_padding",
        "combined gapless source changed");

    Probe::GaplessSkipAccumulator shortSideData(0, 0);
    shortSideData.observe(Probe::GaplessSkipObservation{true, sideData.data(), 8});
    const Probe::GaplessSkipSampleScan shortResult = shortSideData.finalize();
    expect(shortResult.audioPacketsScanned == 1, "short side data lost packet count");
    expect(shortResult.sideDataPacketCount == 0, "short side data was consumed");
    expect(shortResult.skipSamplesTotal == 0, "short side data changed skip total");
}

void testOverflowAndPartialFailure() {
    const std::int64_t maxValue = (std::numeric_limits<std::int64_t>::max)();
    expect(
        Probe::saturatingAddSkipSamples(maxValue - 3, 10) == maxValue,
        "skip arithmetic did not saturate");

    Probe::PacketFrameCountAccumulator partial(44100, AV_CODEC_ID_AAC, AVRational{1, 44100});
    observeSelected(partial, 0, 1024);
    const Probe::PacketFrameCountScan result = partial.finalize("packet frame-count scan read failed: test");
    expect(result.audioPacketCount == 1, "partial diagnostic evidence was lost");
    expect(!result.warning.empty(), "partial read failure did not reject packet authority");

    Probe::GaplessSkipAccumulator partialGapless(0, 0);
    partialGapless.observe(Probe::GaplessSkipObservation{true, nullptr, 0});
    const Probe::GaplessSkipSampleScan gaplessResult =
        partialGapless.finalize("gapless side-data scan read failed: test");
    expect(gaplessResult.audioPacketsScanned == 1, "partial gapless evidence was lost");
    expect(!gaplessResult.warning.empty(), "partial read failure did not reject gapless authority");
}

Probe::ExactPacketPresentationEvidence exactEvidenceFixture() {
    Probe::ExactPacketPresentationEvidence evidence;
    evidence.packetScanReachedEof = true;
    evidence.physicalFrameCountKnown = true;
    evidence.physicalFrameCountExact = true;
    evidence.physicalFrames = 115201024;
    evidence.initialSkipKnown = true;
    evidence.initialSkipAuthoritative = true;
    evidence.initialSkipFrames = 1024;
    evidence.terminalDiscardKnown = true;
    evidence.terminalDiscardAuthoritative = true;
    return evidence;
}

void expectExactBudgetRejected(
    Probe::ExactPacketPresentationEvidence evidence,
    const char* message) {
    expect(!Probe::resolveExactPacketPresentationBudget(evidence).accepted, message);
}

void testExactPacketPresentationBudget() {
    const Probe::ExactPacketPresentationEvidence positive = exactEvidenceFixture();
    const Probe::ExactPacketPresentationBudget accepted =
        Probe::resolveExactPacketPresentationBudget(positive);
    expect(accepted.accepted, "complete exact packet presentation evidence was rejected");
    expect(accepted.presentationFrames == 115200000, "exact packet presentation arithmetic changed");

    auto evidence = positive;
    evidence.physicalFrameCountKnown = false;
    expectExactBudgetRejected(evidence, "unknown physical frame count was accepted");

    evidence = positive;
    evidence.physicalFrameCountExact = false;
    expectExactBudgetRejected(evidence, "inexact physical frame count was accepted");

    evidence = positive;
    evidence.packetScanReachedEof = false;
    expectExactBudgetRejected(evidence, "incomplete packet scan was accepted");

    evidence = positive;
    evidence.packetScanReadError = true;
    expectExactBudgetRejected(evidence, "packet read error was accepted");

    evidence = positive;
    evidence.initialSkipKnown = false;
    expectExactBudgetRejected(evidence, "unknown initial skip was accepted");

    evidence = positive;
    evidence.initialSkipAuthoritative = false;
    expectExactBudgetRejected(evidence, "non-authoritative initial skip was accepted");

    evidence = positive;
    evidence.terminalDiscardKnown = false;
    expectExactBudgetRejected(evidence, "unknown terminal discard was treated as zero");

    evidence = positive;
    evidence.terminalDiscardAuthoritative = false;
    expectExactBudgetRejected(evidence, "non-authoritative terminal discard was accepted");

    evidence = positive;
    evidence.initialSkipFrames = evidence.physicalFrames + 1;
    expectExactBudgetRejected(evidence, "initial skip beyond physical frames was accepted");

    evidence = positive;
    evidence.terminalDiscardFrames = evidence.physicalFrames;
    expectExactBudgetRejected(evidence, "terminal discard beyond remaining frames was accepted");

    evidence = positive;
    evidence.physicalFrames = (std::numeric_limits<std::int64_t>::max)();
    evidence.initialSkipFrames = (std::numeric_limits<std::int64_t>::max)();
    evidence.terminalDiscardFrames = 1;
    expectExactBudgetRejected(evidence, "overflow boundary evidence was accepted");

    evidence = positive;
    evidence.conflictingGaplessEvidence = true;
    expectExactBudgetRejected(evidence, "conflicting gapless evidence was accepted");

    evidence = positive;
    evidence.independentPresentationFramesKnown = true;
    evidence.independentPresentationFrames = 115200001;
    expectExactBudgetRejected(evidence, "conflicting sample-exact stream duration was accepted");

    evidence = positive;
    evidence.initialSkipFrames = 0;
    evidence.terminalDiscardFrames = 0;
    const Probe::ExactPacketPresentationBudget noPadding =
        Probe::resolveExactPacketPresentationBudget(evidence);
    expect(noPadding.accepted, "known zero-padding exact packet evidence was rejected");
    expect(noPadding.presentationFrames == evidence.physicalFrames, "zero-padding frame count changed");

    Probe::FrameCountPolicyState existingExact;
    existingExact.decodedSampleFrames = 105840000;
    existingExact.decodedSampleFramesKind = "exact";
    existingExact.decodedSampleFramesTrust = "authoritative";
    Probe::AudioPresentationEvidenceScan unusedScan;
    expect(
        !Probe::applyExactPacketPresentationBudget(existingExact, unusedScan),
        "existing sample-exact duration authority was replaced");
    expect(existingExact.decodedSampleFrames == 105840000, "existing exact frame count changed");
}

Probe::AudioPresentationEvidenceScan mp4Mp3PostScanEvidenceFixture() {
    Probe::AudioPresentationEvidenceScan scan;
    scan.packetTiming.audioPacketCount = 91876;
    scan.packetTiming.packetsWithDuration = 91876;
    scan.packetTiming.packetsWithTimestamp = 91876;
    scan.packetTiming.packetsWithSampleExactDuration = 91876;
    scan.packetTiming.packetPtsMonotonic = true;
    scan.packetTiming.packetPtsSpanFrames = 105841105;
    scan.packetTiming.packetDurationSumFrames = 105841105;
    scan.packetTiming.sampleExactPacketDurationSumFrames = 105841105;
    scan.packetTiming.mp3FrameCountCandidateFrames = 105841152;
    scan.packetTiming.codecFrameCountFrames = 105841152;
    scan.packetTiming.codecFrameCountKnown = true;
    scan.packetTiming.codecFrameCountExact = true;
    scan.packetTiming.reachedEof = true;
    scan.gapless.packetSkipSamplesStart = 1105;
    scan.gapless.skipSamplesStart = 1105;
    scan.gapless.skipSamplesTotal = 1105;
    scan.gapless.sideDataPacketCount = 1;
    scan.gapless.audioPacketsScanned = 91876;
    scan.gapless.source = "packet_side_data_skip_samples";
    scan.gapless.reachedEof = true;
    return scan;
}

void testPostScanExactPresentationAuthority() {
    Probe::FrameCountPolicyState state;
    state.formatName = "mov,mp4,m4a,3gp,3g2,mj2";
    state.selectedAudio.codecId = AV_CODEC_ID_MP3;
    state.selectedAudio.codecName = "mp3";
    state.selectedAudio.sampleRate = 44100;
    state.selectedAudio.channels = 2;
    state.decodedSampleFrames = 105840000;
    state.decodedSampleFramesKind = "estimated";
    state.decodedSampleFramesTrust = "unsafe_estimated";
    state.decodedSampleFramesSource = "stream_duration_estimate";

    constexpr int codecparInitialPaddingBeforeScan = 0;
    constexpr int codecparTrailingPaddingBeforeScan = 0;
    static_assert(codecparInitialPaddingBeforeScan == 0);
    static_assert(codecparTrailingPaddingBeforeScan == 0);
    expect(
        Probe::shouldEvaluateExactPacketPresentationAfterScan(state, true, true, true),
        "post-scan exact authority still depends on preliminary codec padding");

    const Probe::AudioPresentationEvidenceScan scan = mp4Mp3PostScanEvidenceFixture();
    const Probe::ExactPacketPresentationEvidence evidence =
        Probe::makeExactPacketPresentationEvidence(scan);
    expect(evidence.packetScanReachedEof, "complete post-scan traversal was lost");
    expect(evidence.initialSkipFrames == 1105, "post-scan initial skip changed");
    expect(evidence.terminalDiscardFrames == 47, "post-scan terminal discard changed");
    expect(
        Probe::applyExactPacketPresentationBudget(state, scan, 105840000),
        "post-scan exact presentation authority was rejected");
    expect(state.decodedSampleFrames == 105840000, "post-scan exact presentation changed");
    expect(state.decodedSampleFramesKind == "exact", "post-scan authority kind is not exact");
    expect(
        state.decodedSampleFramesSource == "exact_packet_presentation",
        "post-scan authority source changed");

    Probe::applyPacketFrameCountPolicies(state, scan.packetTiming);
    expect(
        state.decodedSampleFrames == 105840000 &&
            state.decodedSampleFramesSource == "exact_packet_presentation",
        "legacy MP3 candidate replaced stronger post-scan authority");

    expect(
        !Probe::shouldEvaluateExactPacketPresentationAfterScan(state, true, true, true),
        "existing exact authority was evaluated again");
    state.decodedSampleFramesKind = "estimated";
    expect(
        !Probe::shouldEvaluateExactPacketPresentationAfterScan(state, false, true, true),
        "disabled full scan authority was evaluated");
    expect(
        !Probe::shouldEvaluateExactPacketPresentationAfterScan(state, true, false, true),
        "missing packet scan was treated as complete evidence");
    expect(
        !Probe::shouldEvaluateExactPacketPresentationAfterScan(state, true, true, false),
        "missing gapless scan was treated as complete evidence");

    Probe::AudioPresentationEvidenceScan oneSidedLegacy = scan;
    oneSidedLegacy.packetTiming.packetsWithSampleExactDuration = 0;
    oneSidedLegacy.packetTiming.sampleExactPacketDurationSumFrames = 0;
    const Probe::ExactPacketPresentationEvidence oneSidedEvidence =
        Probe::makeExactPacketPresentationEvidence(oneSidedLegacy);
    expect(
        !oneSidedEvidence.terminalDiscardKnown,
        "one-sided legacy MP3 evidence invented terminal authority");
    expectExactBudgetRejected(
        oneSidedEvidence,
        "one-sided legacy MP3 evidence was promoted to exact");
}

void testExactEvidenceDerivation() {
    Probe::AudioPresentationEvidenceScan matroska;
    matroska.packetTiming.audioPacketCount = 112501;
    matroska.packetTiming.packetsWithSampleExactDuration = 112501;
    matroska.packetTiming.sampleExactPacketDurationSumFrames = 113401008;
    matroska.packetTiming.packetPtsSpanFrames = 115201008;
    matroska.packetTiming.codecFrameCountFrames = 115201024;
    matroska.packetTiming.codecFrameCountKnown = true;
    matroska.packetTiming.codecFrameCountExact = true;
    matroska.packetTiming.reachedEof = true;
    matroska.gapless.audioPacketsScanned = 112501;
    matroska.gapless.sideDataPacketCount = 1;
    matroska.gapless.streamInitialPadding = 1024;
    matroska.gapless.skipSamplesStart = 1024;
    matroska.gapless.source = "packet_side_data_skip_samples_and_stream_padding";
    matroska.gapless.reachedEof = true;

    const Probe::ExactPacketPresentationEvidence matroskaEvidence =
        Probe::makeExactPacketPresentationEvidence(matroska);
    expect(matroskaEvidence.initialSkipFrames == 1024, "Matroska start skip evidence changed");
    expect(matroskaEvidence.terminalDiscardFrames == 0, "rounded packet durations invented a tail trim");
    expect(
        Probe::resolveExactPacketPresentationBudget(matroskaEvidence).presentationFrames == 115200000,
        "Matroska exact presentation count changed");

    Probe::AudioPresentationEvidenceScan mov = matroska;
    mov.packetTiming.audioPacketCount = 103361;
    mov.packetTiming.packetsWithSampleExactDuration = 103361;
    mov.packetTiming.sampleExactPacketDurationSumFrames = 105841024;
    mov.packetTiming.packetPtsSpanFrames = 105841024;
    mov.packetTiming.codecFrameCountFrames = 105841664;
    mov.gapless.audioPacketsScanned = 103361;
    const Probe::ExactPacketPresentationEvidence movEvidence =
        Probe::makeExactPacketPresentationEvidence(mov);
    expect(movEvidence.terminalDiscardFrames == 640, "short terminal packet duration was ignored");
    expect(
        Probe::resolveExactPacketPresentationBudget(movEvidence).presentationFrames == 105840000,
        "sample-exact MOV presentation count changed");

    matroska.packetTiming.reachedEof = false;
    expect(
        !Probe::resolveExactPacketPresentationBudget(
             Probe::makeExactPacketPresentationEvidence(matroska)).accepted,
        "partial real scan evidence was promoted");
}

void testWarningsAndOldApiParity(const std::filesystem::path& fixture) {
    const std::filesystem::path missing = fixture.parent_path() / "missing_presentation_scan_input.wav";
    const Probe::AudioPresentationEvidenceScan missingResult =
        Probe::scanAudioPresentationEvidence(
            missing.string(),
            0,
            8000,
            AV_CODEC_ID_PCM_S16LE,
            kScanOptions);
    expect(
        missingResult.packetTiming.warning.rfind("packet frame-count scan open failed:", 0) == 0,
        "packet open warning changed");
    expect(
        missingResult.gapless.warning.rfind("gapless side-data scan open failed:", 0) == 0,
        "gapless open warning changed");
    expect(
        missingResult.packetTiming.warning != missingResult.gapless.warning,
        "component warnings were merged");
    expect(
        packetScansEqual(
            missingResult.packetTiming,
            Probe::scanPacketFrameCountCandidates(
                missing.string(),
                0,
                8000,
                AV_CODEC_ID_PCM_S16LE,
                kScanOptions)),
        "packet open-failure warning parity changed");
    expect(
        gaplessScansEqual(
            missingResult.gapless,
            Probe::scanGaplessSkipSampleSideData(missing.string(), 0, kScanOptions)),
        "gapless open-failure warning parity changed");

    const Probe::AudioPresentationEvidenceScan wrongStream =
        Probe::scanAudioPresentationEvidence(
            fixture.string(),
            99,
            8000,
            AV_CODEC_ID_PCM_S16LE,
            kScanOptions);
    expect(
        wrongStream.packetTiming.warning == "packet frame-count scan audio stream index mismatch",
        "packet stream-index warning changed");
    expect(
        wrongStream.gapless.warning == "gapless side-data scan audio stream index mismatch",
        "gapless stream-index warning changed");
    expect(
        packetScansEqual(
            wrongStream.packetTiming,
            Probe::scanPacketFrameCountCandidates(
                fixture.string(),
                99,
                8000,
                AV_CODEC_ID_PCM_S16LE,
                kScanOptions)),
        "packet stream-index warning parity changed");
    expect(
        gaplessScansEqual(
            wrongStream.gapless,
            Probe::scanGaplessSkipSampleSideData(fixture.string(), 99, kScanOptions)),
        "gapless stream-index warning parity changed");

    const Probe::AudioPresentationEvidenceScan combined =
        Probe::scanAudioPresentationEvidence(
            fixture.string(),
            0,
            8000,
            AV_CODEC_ID_PCM_S16LE,
            kScanOptions);
    const Probe::PacketFrameCountScan oldPacket =
        Probe::scanPacketFrameCountCandidates(
            fixture.string(),
            0,
            8000,
            AV_CODEC_ID_PCM_S16LE,
            kScanOptions);
    const Probe::GaplessSkipSampleScan oldGapless =
        Probe::scanGaplessSkipSampleSideData(fixture.string(), 0, kScanOptions);
    expect(packetScansEqual(combined.packetTiming, oldPacket), "old packet API parity changed");
    expect(gaplessScansEqual(combined.gapless, oldGapless), "old gapless API parity changed");
}

void runUnitTests() {
    const Probe::AudioPresentationEvidenceScan invalid =
        Probe::scanAudioPresentationEvidence({}, -1, 0, AV_CODEC_ID_NONE, {});
    expect(invalid.packetTiming.warning.empty(), "invalid packet input warning changed");
    expect(invalid.gapless.warning.empty(), "invalid gapless input warning changed");

    testPacketTimingAccumulator();
    testNonMonotonicPts();
    testWrappedPacketDurationIsDiagnosticOnly();
    testCodecCandidates();
    testGaplessAccumulator();
    testOverflowAndPartialFailure();
    testExactPacketPresentationBudget();
    testExactEvidenceDerivation();
    testPostScanExactPresentationAuthority();

    const std::filesystem::path fixture = writeDeterministicWaveFixture();
    try {
        testWarningsAndOldApiParity(fixture);
    } catch (...) {
        std::filesystem::remove(fixture);
        throw;
    }
    std::filesystem::remove(fixture);

    std::cout << "AVEMEDIABRIDGE_COMBINED_PRESENTATION_SCAN_PARITY_OK\n";
    std::cout << "AVEMEDIABRIDGE_EXACT_PACKET_PRESENTATION_BUDGET_OK\n";
    std::cout << "AVEMEDIABRIDGE_POST_SCAN_EXACT_PRESENTATION_AUTHORITY_OK\n";
    std::cout << "AVEMEDIABRIDGE_AUDIO_PRESENTATION_EVIDENCE_SCAN_OK\n";
}

struct AudioStreamInfo {
    int index = -1;
    int sampleRate = 0;
    AVCodecID codecId = AV_CODEC_ID_NONE;
    std::int64_t duration = 0;
    AVRational timeBase{0, 1};
};

std::optional<AudioStreamInfo> inspectAudioStream(const std::filesystem::path& path) {
    AVFormatContext* rawContext = nullptr;
    if (avformat_open_input(&rawContext, path.u8string().c_str(), nullptr, nullptr) < 0) {
        return std::nullopt;
    }
    Ffmpeg::UniqueAVFormatContext context(rawContext);
    if (avformat_find_stream_info(context.get(), nullptr) < 0) {
        return std::nullopt;
    }
    const int index = av_find_best_stream(context.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (index < 0 || !context->streams[index] || !context->streams[index]->codecpar) {
        return std::nullopt;
    }
    const AVCodecParameters* codecpar = context->streams[index]->codecpar;
    const AVStream* stream = context->streams[index];
    return AudioStreamInfo{
        index,
        codecpar->sample_rate,
        codecpar->codec_id,
        stream->duration,
        stream->time_base
    };
}

std::int64_t exactStreamDurationFrames(const AudioStreamInfo& info) {
    if (info.duration <= 0 ||
        info.timeBase.num <= 0 ||
        info.timeBase.den <= 0 ||
        info.sampleRate <= 0) {
        return 0;
    }
    const AVRational sampleTimeBase{1, info.sampleRate};
    const std::int64_t frames = av_rescale_q(info.duration, info.timeBase, sampleTimeBase);
    return frames > 0 &&
            av_compare_ts(info.duration, info.timeBase, frames, sampleTimeBase) == 0
        ? frames
        : 0;
}

std::optional<std::int64_t> resolvePresentationBudget(
    const std::optional<AudioStreamInfo>& info,
    const Probe::PacketFrameCountScan& packet,
    const Probe::GaplessSkipSampleScan& gapless) {
    if (!info) {
        return std::nullopt;
    }
    const std::int64_t streamFrames = exactStreamDurationFrames(*info);
    if (streamFrames <= 0 ||
        !packet.warning.empty() ||
        !gapless.warning.empty() ||
        packet.audioPacketCount <= 0 ||
        packet.packetsWithDuration != packet.audioPacketCount ||
        packet.packetDurationSumFrames <= 0) {
        return std::nullopt;
    }

    std::int64_t packetPresentationFrames = packet.packetDurationSumFrames;
    if (gapless.skipSamplesStart > 0) {
        if (packetPresentationFrames <= gapless.skipSamplesStart) {
            return std::nullopt;
        }
        packetPresentationFrames -= gapless.skipSamplesStart;
    }
    if (gapless.skipSamplesEnd > 0) {
        if (packetPresentationFrames <= gapless.skipSamplesEnd) {
            return std::nullopt;
        }
        packetPresentationFrames -= gapless.skipSamplesEnd;
    }
    return packetPresentationFrames == streamFrames
        ? std::optional<std::int64_t>{streamFrames}
        : std::nullopt;
}

bool isCorpusMedia(const std::filesystem::path& path) {
    const std::string name = path.filename().u8string();
    if (path.extension() == ".pkf") {
        return false;
    }
    if (name.rfind("6. ", 0) == 0 && path.extension() == ".mp4") {
        return true;
    }
    if (name.size() < 3 || name[2] != '_') {
        return false;
    }
    if (name[0] < '0' || name[0] > '6' || name[1] < '0' || name[1] > '9') {
        return false;
    }
    const int index = (name[0] - '0') * 10 + (name[1] - '0');
    return index >= 1 && index <= 69;
}

std::vector<std::filesystem::path> corpusFiles(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.is_regular_file() && isCorpusMedia(entry.path())) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

int runCorpusParity(const std::filesystem::path& root) {
    const std::vector<std::filesystem::path> files = corpusFiles(root);
    std::size_t mismatches = 0;
    std::size_t budgetMismatches = 0;
    for (const std::filesystem::path& path : files) {
        const std::optional<AudioStreamInfo> info = inspectAudioStream(path);
        const int index = info ? info->index : -1;
        const int sampleRate = info ? info->sampleRate : 0;
        const AVCodecID codecId = info ? info->codecId : AV_CODEC_ID_NONE;
        const std::string mediaPath = path.u8string();

        const Probe::PacketFrameCountScan oldPacket =
            Probe::scanPacketFrameCountCandidates(
                mediaPath,
                index,
                sampleRate,
                codecId,
                kScanOptions);
        const Probe::GaplessSkipSampleScan oldGapless =
            Probe::scanGaplessSkipSampleSideData(mediaPath, index, kScanOptions);
        const Probe::AudioPresentationEvidenceScan combined =
            Probe::scanAudioPresentationEvidence(
                mediaPath,
                index,
                sampleRate,
                codecId,
                kScanOptions);
        const bool packetEqual = packetScansEqual(oldPacket, combined.packetTiming);
        const bool gaplessEqual = gaplessScansEqual(oldGapless, combined.gapless);
        const std::optional<std::int64_t> oldBudget =
            resolvePresentationBudget(info, oldPacket, oldGapless);
        const std::optional<std::int64_t> combinedBudget =
            resolvePresentationBudget(info, combined.packetTiming, combined.gapless);
        const bool budgetEqual = oldBudget == combinedBudget;
        if (!budgetEqual) {
            ++budgetMismatches;
        }
        if (!packetEqual || !gaplessEqual || !budgetEqual) {
            ++mismatches;
            std::cout << "mismatchFile=" << path.filename().u8string()
                      << " packetEqual=" << (packetEqual ? "yes" : "no")
                      << " gaplessEqual=" << (gaplessEqual ? "yes" : "no")
                      << " budgetEqual=" << (budgetEqual ? "yes" : "no") << '\n';
        }
        if (path.filename().u8string().rfind("6. ", 0) == 0) {
            std::cout << "problemMp4OldBudgetKnown=" << (oldBudget ? "yes" : "no")
                      << " problemMp4NewBudgetKnown=" << (combinedBudget ? "yes" : "no")
                      << " problemMp4OldBudgetFrames=" << oldBudget.value_or(0)
                      << " problemMp4NewBudgetFrames=" << combinedBudget.value_or(0) << '\n';
        }
    }

    std::cout << "filesCompared=" << files.size() << '\n';
    std::cout << "mismatchCount=" << mismatches << '\n';
    std::cout << "presentationBudgetsExact=" << files.size() - budgetMismatches << '\n';
    if (files.size() != 70 || mismatches != 0 || budgetMismatches != 0) {
        return 1;
    }
    std::cout << "AVEMEDIABRIDGE_COMBINED_PRESENTATION_SCAN_CORPUS_PARITY_OK\n";
    return 0;
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

template <typename Function>
double measureMilliseconds(Function&& function) {
    const auto begin = std::chrono::steady_clock::now();
    function();
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

std::filesystem::path findCorpusFile(
    const std::vector<std::filesystem::path>& files,
    const std::string& prefix) {
    const auto found = std::find_if(files.begin(), files.end(), [&](const auto& path) {
        return path.filename().u8string().rfind(prefix, 0) == 0;
    });
    return found == files.end() ? std::filesystem::path{} : *found;
}

int runBenchmark(const std::filesystem::path& root) {
    const std::vector<std::filesystem::path> files = corpusFiles(root);
    const std::array<std::string, 7> prefixes{"10_", "22_", "05_", "08_", "6. ", "06_", "02_"};
    for (const std::string& prefix : prefixes) {
        const std::filesystem::path path = findCorpusFile(files, prefix);
        expect(!path.empty(), "benchmark corpus file is missing");
        const std::optional<AudioStreamInfo> info = inspectAudioStream(path);
        expect(info.has_value(), "benchmark audio stream is unavailable");

        std::vector<double> packetTimes;
        std::vector<double> gaplessTimes;
        std::vector<double> combinedTimes;
        for (int run = 0; run < 5; ++run) {
            const std::string mediaPath = path.u8string();
            packetTimes.push_back(measureMilliseconds([&] {
                (void)Probe::scanPacketFrameCountCandidates(
                    mediaPath,
                    info->index,
                    info->sampleRate,
                    info->codecId,
                    kScanOptions);
            }));
            gaplessTimes.push_back(measureMilliseconds([&] {
                (void)Probe::scanGaplessSkipSampleSideData(mediaPath, info->index, kScanOptions);
            }));
            combinedTimes.push_back(measureMilliseconds([&] {
                (void)Probe::scanAudioPresentationEvidence(
                    mediaPath,
                    info->index,
                    info->sampleRate,
                    info->codecId,
                    kScanOptions);
            }));
        }

        const double packetMs = median(packetTimes);
        const double gaplessMs = median(gaplessTimes);
        const double oldTotalMs = packetMs + gaplessMs;
        const double combinedMs = median(combinedTimes);
        std::cout << "benchmarkFile=" << path.filename().u8string()
                  << " oldPacketScanMs=" << packetMs
                  << " oldGaplessScanMs=" << gaplessMs
                  << " oldCombinedTotalMs=" << oldTotalMs
                  << " newCombinedScanMs=" << combinedMs
                  << " savedMs=" << oldTotalMs - combinedMs
                  << " scanStageSpeedupRatio=" << oldTotalMs / combinedMs << '\n';
    }
    std::cout << "AVEMEDIABRIDGE_COMBINED_PRESENTATION_SCAN_BENCHMARK_OK\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1) {
            runUnitTests();
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--corpus-parity") {
            return runCorpusParity(std::filesystem::u8path(argv[2]));
        }
        if (argc == 3 && std::string(argv[1]) == "--benchmark") {
            return runBenchmark(std::filesystem::u8path(argv[2]));
        }
        std::cerr << "Usage: AveMediaBridgeAudioPresentationEvidenceScanTests "
                     "[--corpus-parity <root> | --benchmark <root>]\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Audio presentation evidence scan test failed: " << error.what() << '\n';
        return 1;
    }
}
