#include "AveMediaBridge/AveMediaBridge.hpp"

#include "AveMediaBridge/Export/FfmpegWavAudioExporter.hpp"
#include "AveMediaBridge/Import/FfmpegAudioImporter.hpp"
#include "AveMediaBridge/Process/AudioBufferProcessor.hpp"

namespace AveMediaBridge {

AudioImportResult AveMediaBridge::importAudio(const std::string& path) {
    FfmpegAudioImporter importer;
    return importer.importFile(path);
}

bool AveMediaBridge::exportAudioAsPcm16Wav(const AudioBufferF32& audio, const std::string& outputPath, std::string& error) {
    FfmpegWavAudioExporter exporter;
    return exporter.exportPcm16Wav(audio, outputPath, error);
}

bool AveMediaBridge::transformToWav(const std::string& inputPath, const std::string& outputWavPath, float gain, std::string& error) {
    AudioImportResult imported = importAudio(inputPath);
    if (!imported.hasUsableAudio()) {
        error = imported.error.empty() ? "audio import failed" : imported.error;
        return false;
    }

    AudioBufferProcessor::applyGain(imported.audio, gain);
    return exportAudioAsPcm16Wav(imported.audio, outputWavPath, error);
}

}  // namespace AveMediaBridge
