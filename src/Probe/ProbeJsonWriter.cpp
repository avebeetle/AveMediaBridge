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
    json << "  \"mp3HeaderPresentationStatus\": "
         << jsonString(document.mp3HeaderPresentationStatus) << ",\n";
    json << "  \"mp3HeaderPresentationReason\": "
         << jsonString(document.mp3HeaderPresentationReason) << ",\n";
    json << "  \"mp3HeaderType\": " << jsonString(document.mp3HeaderType) << ",\n";
    json << "  \"mp3HeaderEncoderProfile\": "
         << jsonString(document.mp3HeaderEncoderProfile) << ",\n";
    json << "  \"mp3HeaderBudgetBytes\": " << document.mp3HeaderBudgetBytes << ",\n";
    json << "  \"mp3HeaderActualReadBytes\": " << document.mp3HeaderActualReadBytes << ",\n";
    json << "  \"mp3HeaderUniqueBytesRead\": " << document.mp3HeaderUniqueBytesRead << ",\n";
    json << "  \"mp3HeaderMaximumBudgetOverrunBytes\": "
         << document.mp3HeaderMaximumBudgetOverrunBytes << ",\n";
    json << "  \"mp3HeaderReadCalls\": " << document.mp3HeaderReadCalls << ",\n";
    json << "  \"mp3HeaderSeekCalls\": " << document.mp3HeaderSeekCalls << ",\n";
    json << "  \"mp3HeaderMaximumOffsetReached\": "
         << document.mp3HeaderMaximumOffsetReached << ",\n";
    json << "  \"mp3HeaderPhysicalFrameCount\": "
         << document.mp3HeaderPhysicalFrameCount << ",\n";
    json << "  \"mp3HeaderSamplesPerFrame\": " << document.mp3HeaderSamplesPerFrame << ",\n";
    json << "  \"mp3HeaderPhysicalSampleTotal\": "
         << document.mp3HeaderPhysicalSampleTotal << ",\n";
    json << "  \"mp3HeaderInitialPresentationSkip\": "
         << document.mp3HeaderInitialPresentationSkip << ",\n";
    json << "  \"mp3HeaderTerminalPresentationPadding\": "
         << document.mp3HeaderTerminalPresentationPadding << ",\n";
    json << "  \"mp3HeaderPresentationFrames\": "
         << document.mp3HeaderPresentationFrames << ",\n";
    json << "  \"mp3HeaderFullScanSkipped\": "
         << (document.mp3HeaderFullScanSkipped ? "true" : "false") << ",\n";
    json << "  \"mp4Mp3SampleTableEligible\": " << (document.mp4Mp3SampleTableEligible ? "true" : "false") << ",\n";
    json << "  \"mp4Mp3SampleTableEntered\": " << (document.mp4Mp3SampleTableEntered ? "true" : "false") << ",\n";
    json << "  \"mp4Mp3SampleTableStatus\": " << jsonString(document.mp4Mp3SampleTableStatus) << ",\n";
    json << "  \"mp4Mp3SampleTableReason\": " << jsonString(document.mp4Mp3SampleTableReason) << ",\n";
    json << "  \"mp4Mp3SampleTableSelectedTrackId\": " << document.mp4Mp3SampleTableSelectedTrackId << ",\n";
    json << "  \"mp4Mp3SampleTableSampleRate\": " << document.mp4Mp3SampleTableSampleRate << ",\n";
    json << "  \"mp4Mp3SampleTableChannels\": " << document.mp4Mp3SampleTableChannels << ",\n";
    json << "  \"mp4Mp3SampleTableSelectedSamples\": " << document.mp4Mp3SampleTableSelectedSamples << ",\n";
    json << "  \"mp4Mp3SampleTableSamplesPerMp3Frame\": " << document.mp4Mp3SampleTableSamplesPerMp3Frame << ",\n";
    json << "  \"mp4Mp3SampleTablePhysicalFrames\": " << document.mp4Mp3SampleTablePhysicalFrames << ",\n";
    json << "  \"mp4Mp3SampleTableInitialSkip\": " << document.mp4Mp3SampleTableInitialSkip << ",\n";
    json << "  \"mp4Mp3SampleTableTerminalDiscard\": " << document.mp4Mp3SampleTableTerminalDiscard << ",\n";
    json << "  \"mp4Mp3SampleTableEditedMediaEnd\": " << document.mp4Mp3SampleTableEditedMediaEnd << ",\n";
    json << "  \"mp4Mp3SampleTablePresentationFrames\": " << document.mp4Mp3SampleTablePresentationFrames << ",\n";
    json << "  \"mp4Mp3SampleTableMovieTimescale\": " << document.mp4Mp3SampleTableMovieTimescale << ",\n";
    json << "  \"mp4Mp3SampleTableMediaTimescale\": " << document.mp4Mp3SampleTableMediaTimescale << ",\n";
    json << "  \"mp4Mp3SampleTableEditMediaStart\": " << document.mp4Mp3SampleTableEditMediaStart << ",\n";
    json << "  \"mp4Mp3SampleTableEditPresentationFrames\": " << document.mp4Mp3SampleTableEditPresentationFrames << ",\n";
    json << "  \"mp4Mp3SampleTableFileSizeBytes\": " << document.mp4Mp3SampleTableFileSizeBytes << ",\n";
    json << "  \"mp4Mp3SampleTableFileIndex\": " << document.mp4Mp3SampleTableFileIndex << ",\n";
    json << "  \"mp4Mp3SampleTableLastWriteTime100ns\": " << document.mp4Mp3SampleTableLastWriteTime100ns << ",\n";
    json << "  \"mp4Mp3SampleTableVolumeSerialNumber\": " << document.mp4Mp3SampleTableVolumeSerialNumber << ",\n";
    json << "  \"mp4Mp3SampleTableBudgetBytes\": " << document.mp4Mp3SampleTableBudgetBytes << ",\n";
    json << "  \"mp4Mp3SampleTableBytesReturned\": " << document.mp4Mp3SampleTableBytesReturned << ",\n";
    json << "  \"mp4Mp3SampleTableUniqueBytes\": " << document.mp4Mp3SampleTableUniqueBytes << ",\n";
    json << "  \"mp4Mp3SampleTableDuplicateBytes\": " << document.mp4Mp3SampleTableDuplicateBytes << ",\n";
    json << "  \"mp4Mp3SampleTableMaximumBudgetOverrunBytes\": " << document.mp4Mp3SampleTableMaximumBudgetOverrunBytes << ",\n";
    json << "  \"mp4Mp3SampleTableReadCalls\": " << document.mp4Mp3SampleTableReadCalls << ",\n";
    json << "  \"mp4Mp3SampleTableSeekCalls\": " << document.mp4Mp3SampleTableSeekCalls << ",\n";
    json << "  \"mp4Mp3SampleTableMaximumOffsetReached\": " << document.mp4Mp3SampleTableMaximumOffsetReached << ",\n";
    json << "  \"mp4Mp3SampleTableScanDurationUs\": " << document.mp4Mp3SampleTableScanDurationUs << ",\n";
    json << "  \"mp4Mp3SampleTableMaximumWorkingBufferBytes\": " << document.mp4Mp3SampleTableMaximumWorkingBufferBytes << ",\n";
    json << "  \"mp4Mp3SampleTableBoxesParsed\": " << document.mp4Mp3SampleTableBoxesParsed << ",\n";
    json << "  \"mp4Mp3SampleTableEntriesParsed\": " << document.mp4Mp3SampleTableEntriesParsed << ",\n";
    json << "  \"mp4Mp3SampleTableSelectedChunks\": " << document.mp4Mp3SampleTableSelectedChunks << ",\n";
    json << "  \"mp4Mp3SampleTableMoovOffset\": " << document.mp4Mp3SampleTableMoovOffset << ",\n";
    json << "  \"mp4Mp3SampleTableMoovSize\": " << document.mp4Mp3SampleTableMoovSize << ",\n";
    json << "  \"mp4Mp3SampleTableMoovAtHead\": " << (document.mp4Mp3SampleTableMoovAtHead ? "true" : "false") << ",\n";
    json << "  \"mp4Mp3SampleTableMoovAtTail\": " << (document.mp4Mp3SampleTableMoovAtTail ? "true" : "false") << ",\n";
    json << "  \"mp4Mp3SampleTableSampleInventoryValid\": " << (document.mp4Mp3SampleTableSampleInventoryValid ? "true" : "false") << ",\n";
    json << "  \"mp4Mp3SampleTableChunkMappingValid\": " << (document.mp4Mp3SampleTableChunkMappingValid ? "true" : "false") << ",\n";
    json << "  \"mp4Mp3SampleTableChunkRangesInsideMdat\": " << (document.mp4Mp3SampleTableChunkRangesInsideMdat ? "true" : "false") << ",\n";
    json << "  \"mp4Mp3SampleTableEditListValid\": " << (document.mp4Mp3SampleTableEditListValid ? "true" : "false") << ",\n";
    json << "  \"mp4Mp3SampleTableMp3ProfileValid\": " << (document.mp4Mp3SampleTableMp3ProfileValid ? "true" : "false") << ",\n";
    json << "  \"mp4Mp3SampleTableCheckedArithmeticValid\": " << (document.mp4Mp3SampleTableCheckedArithmeticValid ? "true" : "false") << ",\n";
    json << "  \"mp4Mp3SampleTableGenericScanEntered\": " << (document.mp4Mp3SampleTableGenericScanEntered ? "true" : "false") << ",\n";
    json << "  \"mp4Mp3SampleTableGenericScanSkipped\": " << (document.mp4Mp3SampleTableGenericScanSkipped ? "true" : "false") << ",\n";
    json << "  \"mp4Mp3SampleTablePossibleDoublePass\": " << (document.mp4Mp3SampleTablePossibleDoublePass ? "true" : "false") << ",\n";
    json << "  \"nutBoundedTailStatus\": " << jsonString(document.nutBoundedTailStatus) << ",\n";
    json << "  \"nutBoundedTailReason\": " << jsonString(document.nutBoundedTailReason) << ",\n";
    json << "  \"nutBoundedTailBudgetBytes\": " << document.nutBoundedTailBudgetBytes << ",\n";
    json << "  \"nutBoundedTailActualReadBytes\": "
         << document.nutBoundedTailActualReadBytes << ",\n";
    json << "  \"nutBoundedTailMaximumBudgetOverrunBytes\": "
         << document.nutBoundedTailMaximumBudgetOverrunBytes << ",\n";
    json << "  \"nutBoundedTailPacketsObserved\": "
         << document.nutBoundedTailPacketsObserved << ",\n";
    json << "  \"nutBoundedTailReachedEof\": "
         << (document.nutBoundedTailReachedEof ? "true" : "false") << ",\n";
    json << "  \"oggOpusSequentialEligible\": "
         << (document.oggOpusSequentialEligible ? "true" : "false") << ",\n";
    json << "  \"oggOpusSequentialEntered\": "
         << (document.oggOpusSequentialEntered ? "true" : "false") << ",\n";
    json << "  \"oggOpusSequentialStatus\": "
         << jsonString(document.oggOpusSequentialStatus) << ",\n";
    json << "  \"oggOpusSequentialReason\": "
         << jsonString(document.oggOpusSequentialReason) << ",\n";
    json << "  \"oggOpusSequentialSelectedSerial\": "
         << document.oggOpusSequentialSelectedSerial << ",\n";
    json << "  \"oggOpusSequentialPreSkip\": "
         << document.oggOpusSequentialPreSkip << ",\n";
    json << "  \"oggOpusSequentialPhysicalPacketFrames\": "
         << document.oggOpusSequentialPhysicalPacketFrames << ",\n";
    json << "  \"oggOpusSequentialLastPacketDuration\": "
         << document.oggOpusSequentialLastPacketDuration << ",\n";
    json << "  \"oggOpusSequentialEosGranule\": "
         << document.oggOpusSequentialEosGranule << ",\n";
    json << "  \"oggOpusSequentialTerminalDiscard\": "
         << document.oggOpusSequentialTerminalDiscard << ",\n";
    json << "  \"oggOpusSequentialPresentationFrames\": "
         << document.oggOpusSequentialPresentationFrames << ",\n";
    json << "  \"oggOpusSequentialFileSizeBytes\": "
         << document.oggOpusSequentialFileSizeBytes << ",\n";
    json << "  \"oggOpusSequentialFileIndex\": "
         << document.oggOpusSequentialFileIndex << ",\n";
    json << "  \"oggOpusSequentialLastWriteTime100ns\": "
         << document.oggOpusSequentialLastWriteTime100ns << ",\n";
    json << "  \"oggOpusSequentialVolumeSerialNumber\": "
         << document.oggOpusSequentialVolumeSerialNumber << ",\n";
    json << "  \"oggOpusSequentialBytesReturned\": "
         << document.oggOpusSequentialBytesReturned << ",\n";
    json << "  \"oggOpusSequentialUniqueBytes\": "
         << document.oggOpusSequentialUniqueBytes << ",\n";
    json << "  \"oggOpusSequentialDuplicateBytes\": "
         << document.oggOpusSequentialDuplicateBytes << ",\n";
    json << "  \"oggOpusSequentialReadCalls\": "
         << document.oggOpusSequentialReadCalls << ",\n";
    json << "  \"oggOpusSequentialSeekCallsAfterOpen\": "
         << document.oggOpusSequentialSeekCallsAfterOpen << ",\n";
    json << "  \"oggOpusSequentialScanDurationUs\": "
         << document.oggOpusSequentialScanDurationUs << ",\n";
    json << "  \"oggOpusSequentialPagesParsed\": "
         << document.oggOpusSequentialPagesParsed << ",\n";
    json << "  \"oggOpusSequentialSelectedPages\": "
         << document.oggOpusSequentialSelectedPages << ",\n";
    json << "  \"oggOpusSequentialSelectedAudioPackets\": "
         << document.oggOpusSequentialSelectedAudioPackets << ",\n";
    json << "  \"oggOpusSequentialMaximumPacketBytes\": "
         << document.oggOpusSequentialMaximumPacketBytes << ",\n";
    json << "  \"oggOpusSequentialMaximumWorkingBufferBytes\": "
         << document.oggOpusSequentialMaximumWorkingBufferBytes << ",\n";
    json << "  \"oggOpusSequentialReachedEof\": "
         << (document.oggOpusSequentialReachedEof ? "true" : "false") << ",\n";
    json << "  \"oggOpusSequentialAllPageCrcValid\": "
         << (document.oggOpusSequentialAllPageCrcValid ? "true" : "false") << ",\n";
    json << "  \"oggOpusSequentialSelectedSequenceContinuous\": "
         << (document.oggOpusSequentialSelectedSequenceContinuous ? "true" : "false")
         << ",\n";
    json << "  \"oggOpusSequentialPacketContinuityValid\": "
         << (document.oggOpusSequentialPacketContinuityValid ? "true" : "false")
         << ",\n";
    json << "  \"oggOpusSequentialFinalGranuleInPacketInterval\": "
         << (document.oggOpusSequentialFinalGranuleInPacketInterval ? "true" : "false")
         << ",\n";
    json << "  \"oggOpusSequentialGenericScanEntered\": "
         << (document.oggOpusSequentialGenericScanEntered ? "true" : "false") << ",\n";
    json << "  \"oggOpusSequentialGenericScanSkipped\": "
         << (document.oggOpusSequentialGenericScanSkipped ? "true" : "false") << ",\n";
    json << "  \"oggOpusSequentialPossibleDoublePass\": "
         << (document.oggOpusSequentialPossibleDoublePass ? "true" : "false") << ",\n";
    json << "  \"matroskaAacSequentialEligible\": "
         << (document.matroskaAacSequentialEligible ? "true" : "false") << ",\n";
    json << "  \"matroskaAacSequentialEntered\": "
         << (document.matroskaAacSequentialEntered ? "true" : "false") << ",\n";
    json << "  \"matroskaAacSequentialStatus\": "
         << jsonString(document.matroskaAacSequentialStatus) << ",\n";
    json << "  \"matroskaAacSequentialReason\": "
         << jsonString(document.matroskaAacSequentialReason) << ",\n";
    json << "  \"matroskaAacSequentialTrackNumber\": "
         << document.matroskaAacSequentialTrackNumber << ",\n";
    json << "  \"matroskaAacSequentialTrackUid\": "
         << document.matroskaAacSequentialTrackUid << ",\n";
    json << "  \"matroskaAacSequentialAacObjectType\": "
         << document.matroskaAacSequentialAacObjectType << ",\n";
    json << "  \"matroskaAacSequentialSampleRate\": "
         << document.matroskaAacSequentialSampleRate << ",\n";
    json << "  \"matroskaAacSequentialChannels\": "
         << document.matroskaAacSequentialChannels << ",\n";
    json << "  \"matroskaAacSequentialSamplesPerAccessUnit\": "
         << document.matroskaAacSequentialSamplesPerAccessUnit << ",\n";
    json << "  \"matroskaAacSequentialSelectedAccessUnits\": "
         << document.matroskaAacSequentialSelectedAccessUnits << ",\n";
    json << "  \"matroskaAacSequentialPhysicalFrames\": "
         << document.matroskaAacSequentialPhysicalFrames << ",\n";
    json << "  \"matroskaAacSequentialCodecDelayNs\": "
         << document.matroskaAacSequentialCodecDelayNs << ",\n";
    json << "  \"matroskaAacSequentialInitialSkipFrames\": "
         << document.matroskaAacSequentialInitialSkipFrames << ",\n";
    json << "  \"matroskaAacSequentialDiscardPaddingNs\": "
         << document.matroskaAacSequentialDiscardPaddingNs << ",\n";
    json << "  \"matroskaAacSequentialTerminalDiscardFrames\": "
         << document.matroskaAacSequentialTerminalDiscardFrames << ",\n";
    json << "  \"matroskaAacSequentialPresentationFrames\": "
         << document.matroskaAacSequentialPresentationFrames << ",\n";
    json << "  \"matroskaAacSequentialFileSizeBytes\": "
         << document.matroskaAacSequentialFileSizeBytes << ",\n";
    json << "  \"matroskaAacSequentialBytesReturned\": "
         << document.matroskaAacSequentialBytesReturned << ",\n";
    json << "  \"matroskaAacSequentialReadCalls\": "
         << document.matroskaAacSequentialReadCalls << ",\n";
    json << "  \"matroskaAacSequentialScanDurationUs\": "
         << document.matroskaAacSequentialScanDurationUs << ",\n";
    json << "  \"matroskaAacSequentialMaximumWorkingBufferBytes\": "
         << document.matroskaAacSequentialMaximumWorkingBufferBytes << ",\n";
    json << "  \"matroskaAacSequentialElementsParsed\": "
         << document.matroskaAacSequentialElementsParsed << ",\n";
    json << "  \"matroskaAacSequentialClustersParsed\": "
         << document.matroskaAacSequentialClustersParsed << ",\n";
    json << "  \"matroskaAacSequentialSelectedBlocks\": "
         << document.matroskaAacSequentialSelectedBlocks << ",\n";
    json << "  \"matroskaAacSequentialSelectedLaces\": "
         << document.matroskaAacSequentialSelectedLaces << ",\n";
    json << "  \"matroskaAacSequentialReachedEof\": "
         << (document.matroskaAacSequentialReachedEof ? "true" : "false") << ",\n";
    json << "  \"matroskaAacSequentialReachedSegmentEnd\": "
         << (document.matroskaAacSequentialReachedSegmentEnd ? "true" : "false")
         << ",\n";
    json << "  \"matroskaAacSequentialTrackMappingValid\": "
         << (document.matroskaAacSequentialTrackMappingValid ? "true" : "false")
         << ",\n";
    json << "  \"matroskaAacSequentialTimestampContinuityValid\": "
         << (document.matroskaAacSequentialTimestampContinuityValid ? "true" : "false")
         << ",\n";
    json << "  \"matroskaAacSequentialAllRelevantCrcValid\": "
         << (document.matroskaAacSequentialAllRelevantCrcValid ? "true" : "false")
         << ",\n";
    json << "  \"matroskaAacSequentialCheckedArithmeticValid\": "
         << (document.matroskaAacSequentialCheckedArithmeticValid ? "true" : "false")
         << ",\n";
    json << "  \"matroskaAacSequentialGenericScanEntered\": "
         << (document.matroskaAacSequentialGenericScanEntered ? "true" : "false")
         << ",\n";
    json << "  \"matroskaAacSequentialGenericScanSkipped\": "
         << (document.matroskaAacSequentialGenericScanSkipped ? "true" : "false")
         << ",\n";
    json << "  \"matroskaAacSequentialPossibleDoublePass\": "
         << (document.matroskaAacSequentialPossibleDoublePass ? "true" : "false")
         << ",\n";
    json << "  \"matroskaAacSequentialLateFallback\": "
         << (document.matroskaAacSequentialLateFallback ? "true" : "false")
         << ",\n";
    json << "  \"adtsAacSequentialEligible\": "
         << (document.adtsAacSequentialEligible ? "true" : "false") << ",\n";
    json << "  \"adtsAacSequentialEntered\": "
         << (document.adtsAacSequentialEntered ? "true" : "false") << ",\n";
    json << "  \"adtsAacSequentialStatus\": "
         << jsonString(document.adtsAacSequentialStatus) << ",\n";
    json << "  \"adtsAacSequentialReason\": "
         << jsonString(document.adtsAacSequentialReason) << ",\n";
    json << "  \"adtsAacSequentialMpegId\": "
         << document.adtsAacSequentialMpegId << ",\n";
    json << "  \"adtsAacSequentialAudioObjectType\": "
         << document.adtsAacSequentialAudioObjectType << ",\n";
    json << "  \"adtsAacSequentialSampleRate\": "
         << document.adtsAacSequentialSampleRate << ",\n";
    json << "  \"adtsAacSequentialChannels\": "
         << document.adtsAacSequentialChannels << ",\n";
    json << "  \"adtsAacSequentialChannelConfiguration\": "
         << document.adtsAacSequentialChannelConfiguration << ",\n";
    json << "  \"adtsAacSequentialProtectionAbsent\": "
         << (document.adtsAacSequentialProtectionAbsent ? "true" : "false")
         << ",\n";
    json << "  \"adtsAacSequentialFrameCount\": "
         << document.adtsAacSequentialFrameCount << ",\n";
    json << "  \"adtsAacSequentialRawDataBlockCount\": "
         << document.adtsAacSequentialRawDataBlockCount << ",\n";
    json << "  \"adtsAacSequentialSamplesPerRawDataBlock\": "
         << document.adtsAacSequentialSamplesPerRawDataBlock << ",\n";
    json << "  \"adtsAacSequentialPhysicalFrames\": "
         << document.adtsAacSequentialPhysicalFrames << ",\n";
    json << "  \"adtsAacSequentialPresentationFrames\": "
         << document.adtsAacSequentialPresentationFrames << ",\n";
    json << "  \"adtsAacSequentialFileSizeBytes\": "
         << document.adtsAacSequentialFileSizeBytes << ",\n";
    json << "  \"adtsAacSequentialFileIndex\": "
         << document.adtsAacSequentialFileIndex << ",\n";
    json << "  \"adtsAacSequentialLastWriteTime100ns\": "
         << document.adtsAacSequentialLastWriteTime100ns << ",\n";
    json << "  \"adtsAacSequentialVolumeSerialNumber\": "
         << document.adtsAacSequentialVolumeSerialNumber << ",\n";
    json << "  \"adtsAacSequentialBytesReturned\": "
         << document.adtsAacSequentialBytesReturned << ",\n";
    json << "  \"adtsAacSequentialUniqueBytes\": "
         << document.adtsAacSequentialUniqueBytes << ",\n";
    json << "  \"adtsAacSequentialDuplicateBytes\": "
         << document.adtsAacSequentialDuplicateBytes << ",\n";
    json << "  \"adtsAacSequentialReadCalls\": "
         << document.adtsAacSequentialReadCalls << ",\n";
    json << "  \"adtsAacSequentialSeekCallsAfterOpen\": "
         << document.adtsAacSequentialSeekCallsAfterOpen << ",\n";
    json << "  \"adtsAacSequentialMaximumFrameBytes\": "
         << document.adtsAacSequentialMaximumFrameBytes << ",\n";
    json << "  \"adtsAacSequentialScanDurationUs\": "
         << document.adtsAacSequentialScanDurationUs << ",\n";
    json << "  \"adtsAacSequentialMaximumWorkingBufferBytes\": "
         << document.adtsAacSequentialMaximumWorkingBufferBytes << ",\n";
    json << "  \"adtsAacSequentialReachedPhysicalEof\": "
         << (document.adtsAacSequentialReachedPhysicalEof ? "true" : "false")
         << ",\n";
    json << "  \"adtsAacSequentialFrameBoundariesValid\": "
         << (document.adtsAacSequentialFrameBoundariesValid ? "true" : "false")
         << ",\n";
    json << "  \"adtsAacSequentialConfigurationContinuous\": "
         << (document.adtsAacSequentialConfigurationContinuous ? "true" : "false")
         << ",\n";
    json << "  \"adtsAacSequentialOutputDomainValidated\": "
         << (document.adtsAacSequentialOutputDomainValidated ? "true" : "false")
         << ",\n";
    json << "  \"adtsAacSequentialCheckedArithmeticValid\": "
         << (document.adtsAacSequentialCheckedArithmeticValid ? "true" : "false")
         << ",\n";
    json << "  \"adtsAacSequentialFileIdentityStable\": "
         << (document.adtsAacSequentialFileIdentityStable ? "true" : "false")
         << ",\n";
    json << "  \"adtsAacSequentialGenericScanEntered\": "
         << (document.adtsAacSequentialGenericScanEntered ? "true" : "false")
         << ",\n";
    json << "  \"adtsAacSequentialGenericScanSkipped\": "
         << (document.adtsAacSequentialGenericScanSkipped ? "true" : "false")
         << ",\n";
    json << "  \"adtsAacSequentialPossibleDoublePass\": "
         << (document.adtsAacSequentialPossibleDoublePass ? "true" : "false")
         << ",\n";
    json << "  \"adtsAacSequentialLateFallback\": "
         << (document.adtsAacSequentialLateFallback ? "true" : "false")
         << ",\n";
    json << "  \"dolbySequentialEligible\": "
         << (document.dolbySequentialEligible ? "true" : "false") << ",\n";
    json << "  \"dolbySequentialEntered\": "
         << (document.dolbySequentialEntered ? "true" : "false") << ",\n";
    json << "  \"dolbySequentialStatus\": "
         << jsonString(document.dolbySequentialStatus) << ",\n";
    json << "  \"dolbySequentialReason\": "
         << jsonString(document.dolbySequentialReason) << ",\n";
    json << "  \"dolbySequentialCodecFamily\": "
         << jsonString(document.dolbySequentialCodecFamily) << ",\n";
    json << "  \"dolbySequentialSampleRate\": " << document.dolbySequentialSampleRate << ",\n";
    json << "  \"dolbySequentialChannels\": " << document.dolbySequentialChannels << ",\n";
    json << "  \"dolbySequentialBitstreamId\": " << document.dolbySequentialBitstreamId << ",\n";
    json << "  \"dolbySequentialChannelMode\": " << document.dolbySequentialChannelMode << ",\n";
    json << "  \"dolbySequentialLfe\": " << (document.dolbySequentialLfe ? "true" : "false") << ",\n";
    json << "  \"dolbySequentialSelectedStreamType\": " << document.dolbySequentialSelectedStreamType << ",\n";
    json << "  \"dolbySequentialSelectedSubstreamId\": " << document.dolbySequentialSelectedSubstreamId << ",\n";
    json << "  \"dolbySequentialSyncframeCount\": " << document.dolbySequentialSyncframeCount << ",\n";
    json << "  \"dolbySequentialAc3FrameCount\": " << document.dolbySequentialAc3FrameCount << ",\n";
    json << "  \"dolbySequentialEac3IndependentFrameCount\": " << document.dolbySequentialEac3IndependentFrameCount << ",\n";
    json << "  \"dolbySequentialEac3DependentFrameCount\": " << document.dolbySequentialEac3DependentFrameCount << ",\n";
    json << "  \"dolbySequentialAudioBlockCount\": " << document.dolbySequentialAudioBlockCount << ",\n";
    json << "  \"dolbySequentialSamplesPerAudioBlock\": " << document.dolbySequentialSamplesPerAudioBlock << ",\n";
    json << "  \"dolbySequentialPresentationFrames\": " << document.dolbySequentialPresentationFrames << ",\n";
    json << "  \"dolbySequentialFileSizeBytes\": " << document.dolbySequentialFileSizeBytes << ",\n";
    json << "  \"dolbySequentialFileIndex\": " << document.dolbySequentialFileIndex << ",\n";
    json << "  \"dolbySequentialLastWriteTime100ns\": " << document.dolbySequentialLastWriteTime100ns << ",\n";
    json << "  \"dolbySequentialVolumeSerialNumber\": " << document.dolbySequentialVolumeSerialNumber << ",\n";
    json << "  \"dolbySequentialBytesReturned\": " << document.dolbySequentialBytesReturned << ",\n";
    json << "  \"dolbySequentialUniqueBytes\": " << document.dolbySequentialUniqueBytes << ",\n";
    json << "  \"dolbySequentialDuplicateBytes\": " << document.dolbySequentialDuplicateBytes << ",\n";
    json << "  \"dolbySequentialReadCalls\": " << document.dolbySequentialReadCalls << ",\n";
    json << "  \"dolbySequentialSeekCallsAfterOpen\": " << document.dolbySequentialSeekCallsAfterOpen << ",\n";
    json << "  \"dolbySequentialMaximumFrameBytes\": " << document.dolbySequentialMaximumFrameBytes << ",\n";
    json << "  \"dolbySequentialScanDurationUs\": " << document.dolbySequentialScanDurationUs << ",\n";
    json << "  \"dolbySequentialMaximumWorkingBufferBytes\": " << document.dolbySequentialMaximumWorkingBufferBytes << ",\n";
    json << "  \"dolbySequentialReachedPhysicalEof\": " << (document.dolbySequentialReachedPhysicalEof ? "true" : "false") << ",\n";
    json << "  \"dolbySequentialFrameBoundariesValid\": " << (document.dolbySequentialFrameBoundariesValid ? "true" : "false") << ",\n";
    json << "  \"dolbySequentialConfigurationContinuous\": " << (document.dolbySequentialConfigurationContinuous ? "true" : "false") << ",\n";
    json << "  \"dolbySequentialSubstreamPolicyValid\": " << (document.dolbySequentialSubstreamPolicyValid ? "true" : "false") << ",\n";
    json << "  \"dolbySequentialOutputDomainValidated\": " << (document.dolbySequentialOutputDomainValidated ? "true" : "false") << ",\n";
    json << "  \"dolbySequentialCheckedArithmeticValid\": " << (document.dolbySequentialCheckedArithmeticValid ? "true" : "false") << ",\n";
    json << "  \"dolbySequentialFileIdentityStable\": " << (document.dolbySequentialFileIdentityStable ? "true" : "false") << ",\n";
    json << "  \"dolbySequentialCrcObserved\": " << (document.dolbySequentialCrcObserved ? "true" : "false") << ",\n";
    json << "  \"dolbySequentialCrcValidated\": " << (document.dolbySequentialCrcValidated ? "true" : "false") << ",\n";
    json << "  \"dolbySequentialPayloadValiditySeparatedFromExtent\": " << (document.dolbySequentialPayloadValiditySeparatedFromExtent ? "true" : "false") << ",\n";
    json << "  \"dolbySequentialTypedEvidencePublished\": " << (document.dolbySequentialTypedEvidencePublished ? "true" : "false") << ",\n";
    json << "  \"dolbySequentialGenericFullScanEntered\": " << (document.dolbySequentialGenericFullScanEntered ? "true" : "false") << ",\n";
    json << "  \"dolbySequentialGenericFullScanSkipped\": " << (document.dolbySequentialGenericFullScanSkipped ? "true" : "false") << ",\n";
    json << "  \"dolbySequentialDuplicatePresentationScanEntered\": " << (document.dolbySequentialDuplicatePresentationScanEntered ? "true" : "false") << ",\n";
    json << "  \"dolbySequentialPossibleDoublePass\": " << (document.dolbySequentialPossibleDoublePass ? "true" : "false") << ",\n";
    json << "  \"dolbySequentialLateFallback\": " << (document.dolbySequentialLateFallback ? "true" : "false") << ",\n";
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
