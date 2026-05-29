#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace AveMediaBridge {

struct SourceMediaInfo {
    std::string inputPath;
    std::string formatName;
    std::string formatLongName;
    double durationSeconds = 0.0;
    int streamCount = 0;
};

struct StreamSummary {
    int index = -1;
    std::string mediaType;
    std::string codecName;
    int codecId = 0;
    int sampleRate = 0;
    int channels = 0;
    std::int64_t bitRate = 0;
    std::string timeBase;
};

struct SelectedAudioStreamInfo {
    int index = -1;
    std::string codecName;
    int codecId = 0;
    std::string decoderName;
    int sampleRate = 0;
    int channels = 0;
    std::int64_t bitRate = 0;
    std::string timeBase;
    std::string decoderSampleFormat;
};

struct DecodeReport {
    std::int64_t packetsRead = 0;
    std::int64_t audioPackets = 0;
    std::int64_t invalidPacketsSkipped = 0;
    std::int64_t decodedFrames = 0;
    std::int64_t outputSamplesPerChannel = 0;
    std::int64_t outputInterleavedFloatSamples = 0;
    int outputSampleRate = 0;
    int outputChannels = 0;
    std::string outputSampleFormat = "float32 interleaved";
    bool decoderOpened = false;
    bool swrInitialized = false;
    int warningsCount = 0;
    std::vector<std::string> warnings;
};

struct MediaProbeDetails {
    bool streamInfoFound = false;
    int audioStreamCount = 0;
    std::vector<StreamSummary> streams;
};

}  // namespace AveMediaBridge
