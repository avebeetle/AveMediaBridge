#pragma once

#include "ProbeJsonWriter.hpp"
#include "../Ffmpeg/FfmpegHeaders.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace AveMediaBridge::Probe {

struct FastProbeResult {
    FastProbeJsonDocument document;
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
