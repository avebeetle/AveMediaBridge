#pragma once
#include "Export/StreamingExportJob.hpp"
#include "Export/ExportScratchIo.hpp"
namespace AveMediaBridge::Export {
bool floatWavWriterAvailable() noexcept;
std::unique_ptr<IStreamingPcmWriter> makeFloatWavWriter(
    const Path& scratchPath, const AMBE_InputV1& input, bool forceRf64ForTest = false,
    ExportScratchIo::Fault scratchFaultForTest = ExportScratchIo::Fault::None);
}
