#include "Export/StreamingExportJob.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

static_assert(sizeof(AMBE_InputV1) == 48 && offsetof(AMBE_InputV1, expectedFrames) == 24);
static_assert(sizeof(AMBE_CapabilitiesV1) == 32 && offsetof(AMBE_CapabilitiesV1, profileBits) == 8);
static_assert(sizeof(AMBE_ResultV1) == 32 && offsetof(AMBE_ResultV1, acceptedFrames) == 8);

namespace AveMediaBridge::Export {

namespace {
template <std::size_t N>
bool allZero(const uint32_t (&values)[N]) noexcept {
    for (const uint32_t value : values) {
        if (value != 0) return false;
    }
    return true;
}
}

AMBE_Status validateInput(const AMBE_InputV1* input) noexcept {
    if (!input || input->structSize != sizeof(AMBE_InputV1) ||
        input->abiVersion != AMBE_ABI_VERSION || !allZero(input->reserved) ||
        input->profile != AMBE_PROFILE_WAV_F32_NATIVE_V1 ||
        input->sampleRate == 0 || input->sampleRate > INT32_MAX ||
        input->expectedFrames == 0 ||
        !((input->channels == 1 && input->layout == AMBE_LAYOUT_MONO) ||
          (input->channels == 2 && input->layout == AMBE_LAYOUT_STEREO_LR))) {
        return AMBE_INVALID_ARGUMENT;
    }
    const uint64_t bytesPerFrame = static_cast<uint64_t>(input->channels) * sizeof(float);
    if (static_cast<uint64_t>(input->sampleRate) * bytesPerFrame > UINT32_MAX ||
        input->expectedFrames > (static_cast<uint64_t>(INT64_MAX) - 80u) / bytesPerFrame) {
        return AMBE_INVALID_ARGUMENT;
    }
    return AMBE_OK;
}

AMBE_Status validateResult(const AMBE_ResultV1* result) noexcept {
    return result && result->structSize == sizeof(AMBE_ResultV1) &&
        result->abiVersion == AMBE_ABI_VERSION && allZero(result->reserved)
        ? AMBE_OK : AMBE_INVALID_ARGUMENT;
}

AMBE_Status validateCapabilities(const AMBE_CapabilitiesV1* capabilities) noexcept {
    return capabilities && capabilities->structSize == sizeof(AMBE_CapabilitiesV1) &&
        capabilities->abiVersion == AMBE_ABI_VERSION && allZero(capabilities->reserved)
        ? AMBE_OK : AMBE_INVALID_ARGUMENT;
}

StreamingExportJob::StreamingExportJob(AMBE_InputV1 input, std::unique_ptr<IStreamingPcmWriter> writer)
    : input_(input), writer_(std::move(writer)) {}
StreamingExportJob::~StreamingExportJob() {
    abort();
}

AMBE_Status StreamingExportJob::write(const float* samples, uint32_t frameCount) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Writing) return AMBE_INVALID_STATE;
    const uint64_t maxFrames = AMBE_MAX_BLOCK_BYTES /
        (static_cast<uint64_t>(input_.channels) * sizeof(float));
    if (!samples || frameCount == 0 || frameCount > maxFrames ||
        frameCount > input_.expectedFrames - acceptedFrames_) {
        state_ = State::Failed;
        return AMBE_INVALID_ARGUMENT;
    }
    const uint64_t sampleCount = static_cast<uint64_t>(frameCount) * input_.channels;
    for (uint64_t i = 0; i < sampleCount; ++i) {
        if (!std::isfinite(samples[i])) {
            state_ = State::Failed;
            return AMBE_INVALID_ARGUMENT;
        }
    }
    try {
        writer_->write(samples, frameCount);
        acceptedFrames_ += frameCount;
        return AMBE_OK;
    } catch (...) {
        state_ = State::Failed;
        return AMBE_INTERNAL_ERROR;
    }
}

AMBE_Status StreamingExportJob::finish(AMBE_ResultV1* result) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Writing) return AMBE_INVALID_STATE;
    if (validateResult(result) != AMBE_OK) return AMBE_INVALID_ARGUMENT;
    if (acceptedFrames_ != input_.expectedFrames) {
        state_ = State::Failed;
        return AMBE_INVALID_STATE;
    }
    try {
        const uint64_t encodedFrames = writer_->finish();
        if (encodedFrames != input_.expectedFrames) {
            state_ = State::Failed;
            return AMBE_FINALIZE_ERROR;
        }
        result->acceptedFrames = acceptedFrames_;
        result->encodedFrames = encodedFrames;
        state_ = State::Finished;
        return AMBE_OK;
    } catch (...) {
        state_ = State::Failed;
        return AMBE_INTERNAL_ERROR;
    }
}

AMBE_Status StreamingExportJob::abort() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Aborted) return AMBE_OK;
    if (state_ == State::Finished) return AMBE_INVALID_STATE;
    writer_->abort();
    state_ = State::Aborted;
    return AMBE_OK;
}

StreamingExportJob::State StreamingExportJob::state() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

AMBE_Handle ExportRegistry::add(std::shared_ptr<StreamingExportJob> job) {
    if (!job) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (next_ == 0) return 0;
    const AMBE_Handle token = next_++;
    jobs_.emplace(token, std::move(job));
    return token;
}

std::shared_ptr<StreamingExportJob> ExportRegistry::find(AMBE_Handle handle) {
    if (handle == 0) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(handle);
    return it == jobs_.end() ? std::shared_ptr<StreamingExportJob>{} : it->second;
}

AMBE_Status ExportRegistry::destroy(AMBE_Handle handle) noexcept {
    if (handle == 0) return AMBE_INVALID_STATE;
    std::shared_ptr<StreamingExportJob> job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = jobs_.find(handle);
        if (it == jobs_.end()) return AMBE_INVALID_STATE;
        job = std::move(it->second);
        jobs_.erase(it);
    }
    job->abort();
    return AMBE_OK;
}

} // namespace AveMediaBridge::Export
