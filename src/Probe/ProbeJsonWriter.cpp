#include "ProbeJsonWriter.hpp"

#include "../Utils/JsonUtils.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <ostream>

namespace AveMediaBridge::Probe {
namespace {

using AveMediaBridge::Utils::jsonString;

void writeStringArray(std::ostream& out, const std::vector<std::string>& values) {
    out << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << jsonString(values[i]);
    }
    out << "]";
}

void writeSelectedAudioJson(std::ostream& out, const SelectedAudioStreamInfo& stream, const char* indent) {
    out << indent << "{\n";
    out << indent << "  \"index\": " << stream.index << ",\n";
    out << indent << "  \"codecName\": " << jsonString(stream.codecName) << ",\n";
    out << indent << "  \"codecId\": " << stream.codecId << ",\n";
    out << indent << "  \"decoderName\": " << jsonString(stream.decoderName) << ",\n";
    out << indent << "  \"sampleRate\": " << stream.sampleRate << ",\n";
    out << indent << "  \"channels\": " << stream.channels << ",\n";
    out << indent << "  \"bitRate\": " << stream.bitRate << ",\n";
    out << indent << "  \"timeBase\": " << jsonString(stream.timeBase) << ",\n";
    out << indent << "  \"decoderSampleFormat\": " << jsonString(stream.decoderSampleFormat) << "\n";
    out << indent << "}";
}

void writeStreamSummariesJson(std::ostream& out, const std::vector<StreamSummary>& streams, const char* indent) {
    out << indent << "[\n";
    for (std::size_t i = 0; i < streams.size(); ++i) {
        const auto& stream = streams[i];
        out << indent << "  {\n";
        out << indent << "    \"index\": " << stream.index << ",\n";
        out << indent << "    \"mediaType\": " << jsonString(stream.mediaType) << ",\n";
        out << indent << "    \"codecName\": " << jsonString(stream.codecName) << ",\n";
        out << indent << "    \"codecId\": " << stream.codecId << ",\n";
        out << indent << "    \"sampleRate\": " << stream.sampleRate << ",\n";
        out << indent << "    \"channels\": " << stream.channels << ",\n";
        out << indent << "    \"bitRate\": " << stream.bitRate << ",\n";
        out << indent << "    \"timeBase\": " << jsonString(stream.timeBase) << "\n";
        out << indent << "  }" << (i + 1 < streams.size() ? "," : "") << "\n";
    }
    out << indent << "]";
}

bool createParentDirectory(const std::filesystem::path& filePath, std::string& error) {
    const std::filesystem::path parent = filePath.parent_path();
    if (parent.empty()) {
        return true;
    }

    std::error_code fsError;
    std::filesystem::create_directories(parent, fsError);
    if (fsError) {
        error = "failed to create output directory: " + fsError.message();
        return false;
    }
    if (!std::filesystem::is_directory(parent, fsError)) {
        error = "path is not a directory";
        return false;
    }
    if (fsError) {
        error = "failed to inspect directory: " + fsError.message();
        return false;
    }
    return true;
}

}  // namespace

bool writeProbeJson(
    const std::filesystem::path& outputPath,
    const FastProbeJsonDocument& document,
    std::string& error) {
    if (!createParentDirectory(outputPath, error)) {
        return false;
    }

    std::ofstream json(outputPath, std::ios::binary);
    if (!json) {
        error = "failed to open JSON output file";
        return false;
    }

    json << std::fixed << std::setprecision(9);
    json << "{\n";
    json << "  \"apiVersion\": 1,\n";
    json << "  \"schemaVersion\": 2,\n";
    json << "  \"sourcePath\": " << jsonString(document.sourcePath) << ",\n";
    json << "  \"probeMode\": \"fast_v2\",\n";
    json << "  \"hasAudio\": " << (document.hasAudio ? "true" : "false") << ",\n";
    json << "  \"bestAudioStreamIndex\": " << document.bestAudioStreamIndex << ",\n";
    json << "  \"selectedAudioStreamIndex\": " << document.selectedAudio.index << ",\n";
    json << "  \"containerFormat\": " << jsonString(document.containerFormat) << ",\n";
    json << "  \"formatName\": " << jsonString(document.formatName) << ",\n";
    json << "  \"formatLongName\": " << jsonString(document.formatLongName) << ",\n";
    json << "  \"codecName\": " << jsonString(document.selectedAudio.codecName) << ",\n";
    json << "  \"codecId\": " << document.selectedAudio.codecId << ",\n";
    json << "  \"sampleRate\": " << document.selectedAudio.sampleRate << ",\n";
    json << "  \"channels\": " << document.selectedAudio.channels << ",\n";
    json << "  \"channelLayout\": " << jsonString(document.channelLayout) << ",\n";
    json << "  \"frames\": " << document.decodedSampleFrames << ",\n";
    json << "  \"durationSec\": " << document.durationSec << ",\n";
    json << "  \"durationKind\": " << jsonString(document.durationKind) << ",\n";
    json << "  \"durationEstimationMethod\": " << jsonString(document.durationEstimationMethod) << ",\n";
    json << "  \"decodedSampleFrames\": " << document.decodedSampleFrames << ",\n";
    json << "  \"decodedSampleFramesKind\": " << jsonString(document.decodedSampleFramesKind) << ",\n";
    json << "  \"decodedSampleFramesTrust\": " << jsonString(document.decodedSampleFramesTrust) << ",\n";
    json << "  \"decodedSampleFramesSource\": " << jsonString(document.decodedSampleFramesSource) << ",\n";
    json << "  \"decodedSampleFramesBeforeCorrection\": "
         << document.decodedSampleFramesBeforeCorrection << ",\n";
    json << "  \"packetPtsSpanFrames\": " << document.packetPtsSpanFrames << ",\n";
    json << "  \"packetDurationSumFrames\": " << document.packetDurationSumFrames << ",\n";
    json << "  \"packetFrameCountCandidateUsed\": "
         << (document.packetFrameCountCandidateUsed ? "true" : "false") << ",\n";
    json << "  \"frameCountPolicyReason\": "
         << jsonString(document.frameCountPolicyReason) << ",\n";
    json << "  \"decodedSampleFramesBeforeGaplessCorrection\": "
         << document.decodedSampleFramesBeforeGaplessCorrection << ",\n";
    json << "  \"skipSamplesStart\": " << document.skipSamplesStart << ",\n";
    json << "  \"skipSamplesEnd\": " << document.skipSamplesEnd << ",\n";
    json << "  \"skipSamplesTotal\": " << document.skipSamplesTotal << ",\n";
    json << "  \"gaplessCorrectedDecodedSampleFrames\": "
         << document.gaplessCorrectedDecodedSampleFrames << ",\n";
    json << "  \"gaplessCorrectionApplied\": "
         << (document.gaplessCorrectionApplied ? "true" : "false") << ",\n";
    json << "  \"gaplessCorrectionSource\": "
         << jsonString(document.gaplessCorrectionSource) << ",\n";
    json << "  \"gaplessSideDataPacketCount\": "
         << document.gaplessSideDataPacketCount << ",\n";
    json << "  \"gaplessAudioPacketsScanned\": "
         << document.gaplessAudioPacketsScanned << ",\n";
    json << "  \"estimatedDecodedBytes\": " << document.estimatedDecodedBytes << ",\n";
    json << "  \"estimatedDecodedBytesKind\": " << jsonString(document.estimatedDecodedBytesKind) << ",\n";
    json << "  \"probeScore\": " << document.probeScore << ",\n";
    json << "  \"streamCount\": " << document.streamCount << ",\n";
    json << "  \"audioStreamCount\": " << document.audioStreamCount << ",\n";
    json << "  \"selectedAudioStream\": ";
    writeSelectedAudioJson(json, document.selectedAudio, "  ");
    json << ",\n";
    json << "  \"streams\": ";
    writeStreamSummariesJson(json, document.streams, "  ");
    json << ",\n";
    json << "  \"warnings\": ";
    writeStringArray(json, document.warnings);
    json << ",\n";
    json << "  \"errors\": ";
    writeStringArray(json, document.errors);
    json << "\n";
    json << "}\n";

    if (!json) {
        error = "failed to write JSON output file";
        return false;
    }
    return true;
}

}  // namespace AveMediaBridge::Probe
