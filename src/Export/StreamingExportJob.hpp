#pragma once

#include "AveMediaBridge/AveMediaBridgeExportApi.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace AveMediaBridge::Export {

using Path = std::filesystem::path;

class IStreamingPcmWriter {
public:
    virtual ~IStreamingPcmWriter() = default;
    virtual void write(const float* samples, uint32_t frameCount) = 0;
    virtual uint64_t finish() = 0;
    virtual void abort() noexcept = 0;
};

class StreamingExportJob {
public:
    enum class State { Writing, Finished, Failed, Aborted };
    StreamingExportJob(AMBE_InputV1 input, std::unique_ptr<IStreamingPcmWriter> writer);
    ~StreamingExportJob();
    AMBE_Status write(const float* samples, uint32_t frameCount) noexcept;
    AMBE_Status finish(AMBE_ResultV1* result) noexcept;
    AMBE_Status abort() noexcept;
    State state() const noexcept;

private:
    AMBE_InputV1 input_;
    std::unique_ptr<IStreamingPcmWriter> writer_;
    uint64_t acceptedFrames_ = 0;
    State state_ = State::Writing;
    mutable std::mutex mutex_;
};

class ExportRegistry {
public:
    AMBE_Handle add(std::shared_ptr<StreamingExportJob> job);
    std::shared_ptr<StreamingExportJob> find(AMBE_Handle handle);
    AMBE_Status destroy(AMBE_Handle handle) noexcept;

private:
    std::mutex mutex_;
    AMBE_Handle next_ = 1;
    std::unordered_map<AMBE_Handle, std::shared_ptr<StreamingExportJob>> jobs_;
};

AMBE_Status validateInput(const AMBE_InputV1* input) noexcept;
AMBE_Status validateResult(const AMBE_ResultV1* result) noexcept;
AMBE_Status validateCapabilities(const AMBE_CapabilitiesV1* capabilities) noexcept;

} // namespace AveMediaBridge::Export
