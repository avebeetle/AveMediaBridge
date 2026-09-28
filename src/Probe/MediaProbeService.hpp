#pragma once

#include "AdtsAacSequentialPresentation.hpp"
#include "Ac3Eac3SequentialPresentation.hpp"
#include "PresentationBudgetPolicy.hpp"
#include "MatroskaAacSequentialPresentation.hpp"
#include "MediaOpenValidator.hpp"
#include "Mp4Mp3SampleEditTablePresentation.hpp"
#include "Mp3HeaderPresentation.hpp"
#include "NutBoundedTailAuthority.hpp"
#include "OggOpusSequentialPresentation.hpp"
#include "ProbeJsonWriter.hpp"
#include "../Input/SelectedAudioBinding.hpp"
#include "../Ffmpeg/FfmpegHeaders.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace AveMediaBridge::Probe {

struct FastProbeResult {
    FastProbeJsonDocument document;
    TotalPresentationEvidence totalPresentation;
    MediaOpenAssessment mediaOpenAssessment;
    Mp3HeaderPresentationResult mp3HeaderPresentation;
    NutBoundedTailProbeResult nutBoundedTail;
    OggOpusSequentialPresentationResult oggOpusSequentialPresentation;
    MatroskaAacSequentialPresentationResult matroskaAacSequentialPresentation;
    Mp4Mp3SampleEditTablePresentationResult mp4Mp3SampleEditTablePresentation;
    AdtsAacSequentialPresentationResult adtsAacSequentialPresentation;
    DolbySequentialPresentationResult dolbySequentialPresentation;
    bool streamInfoFound = false;
    std::optional<Input::SelectedAudioBinding> stableBinding;
};

std::string rationalToString(AVRational value);
StreamSummary makeStreamSummary(int index, const AVStream* stream);

FastProbeResult runFastProbe(const std::string& path);
FastProbeResult runFastProbe(const Input::MediaInputSource& source);
#ifdef AVEMEDIABRIDGE_TEST_ONLY
FastProbeResult runFastProbeWithTestPadding(const Input::MediaInputSource& source);
#endif

bool writeFastProbeJson(
    const std::filesystem::path& outputPath,
    const FastProbeResult& result,
    std::string& error);

bool estimateDecodedBytesForPreflight(
    const AVFormatContext* formatContext,
    const AVStream* audioStream,
    const Input::MediaInputSource& source,
    std::int64_t& estimatedFrames,
    std::int64_t& estimatedBytes,
    std::string& estimateKind);

bool estimateDecodedBytesForPreflight(
    const AVFormatContext* formatContext,
    const AVStream* audioStream,
    const std::string& path,
    std::int64_t& estimatedFrames,
    std::int64_t& estimatedBytes,
    std::string& estimateKind);

}  // namespace AveMediaBridge::Probe
