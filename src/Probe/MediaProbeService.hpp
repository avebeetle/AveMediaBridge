#pragma once

#include "PresentationBudgetPolicy.hpp"
#include "MatroskaAacSequentialPresentation.hpp"
#include "Mp4Mp3SampleEditTablePresentation.hpp"
#include "Mp3HeaderPresentation.hpp"
#include "NutBoundedTailAuthority.hpp"
#include "OggOpusSequentialPresentation.hpp"
#include "ProbeJsonWriter.hpp"
#include "../Ffmpeg/FfmpegHeaders.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace AveMediaBridge::Probe {

struct FastProbeResult {
    FastProbeJsonDocument document;
    TotalPresentationEvidence totalPresentation;
    Mp3HeaderPresentationResult mp3HeaderPresentation;
    NutBoundedTailProbeResult nutBoundedTail;
    OggOpusSequentialPresentationResult oggOpusSequentialPresentation;
    MatroskaAacSequentialPresentationResult matroskaAacSequentialPresentation;
    Mp4Mp3SampleEditTablePresentationResult mp4Mp3SampleEditTablePresentation;
    bool streamInfoFound = false;
};

std::string rationalToString(AVRational value);
StreamSummary makeStreamSummary(int index, const AVStream* stream);

FastProbeResult runFastProbe(const std::string& path);

bool writeFastProbeJson(
    const std::filesystem::path& outputPath,
    const FastProbeResult& result,
    std::string& error);

bool estimateDecodedBytesForPreflight(
    const AVFormatContext* formatContext,
    const AVStream* audioStream,
    const std::string& path,
    std::int64_t& estimatedFrames,
    std::int64_t& estimatedBytes,
    std::string& estimateKind);

}  // namespace AveMediaBridge::Probe
