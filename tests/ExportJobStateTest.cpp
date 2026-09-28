#include "Export/StreamingExportJob.hpp"

#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace AveMediaBridge::Export;

struct Check {
    bool ok = true;
    void expect(bool condition, const char* message) {
        if (!condition) { std::cerr << "FAIL: " << message << '\n'; ok = false; }
    }
};

struct Capture {
    std::vector<float> samples;
    bool closed = false;
    bool throwWrite = false;
    bool throwFinish = false;
    uint64_t encodedOverride = 0;
};

class FakeWriter final : public IStreamingPcmWriter {
public:
    explicit FakeWriter(std::shared_ptr<Capture> capture) : capture_(std::move(capture)) {}
    void write(const float* samples, uint32_t frames) override {
        if (capture_->throwWrite) throw std::runtime_error("write");
        capture_->samples.insert(capture_->samples.end(), samples, samples + frames * 2);
    }
    uint64_t finish() override {
        if (capture_->throwFinish) throw std::runtime_error("finish");
        capture_->closed = true;
        return capture_->encodedOverride ? capture_->encodedOverride : capture_->samples.size() / 2;
    }
    void abort() noexcept override { capture_->closed = true; }
private:
    std::shared_ptr<Capture> capture_;
};

static AMBE_InputV1 stereo3() {
    AMBE_InputV1 input{};
    input.structSize = sizeof(input); input.abiVersion = AMBE_ABI_VERSION;
    input.sampleRate = 48000; input.channels = 2; input.layout = AMBE_LAYOUT_STEREO_LR;
    input.profile = AMBE_PROFILE_WAV_F32_NATIVE_V1; input.expectedFrames = 3;
    return input;
}

static std::shared_ptr<StreamingExportJob> makeJob(std::shared_ptr<Capture> capture) {
    return std::make_shared<StreamingExportJob>(stereo3(), std::make_unique<FakeWriter>(capture));
}

int main() {
    Check check;
    const float pcm[] = {0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f, 0.4f, -0.4f};
    AMBE_ResultV1 result{}; result.structSize = sizeof(result); result.abiVersion = AMBE_ABI_VERSION;

    auto capture = std::make_shared<Capture>();
    auto job = makeJob(capture);
    check.expect(job->write(pcm, 2) == AMBE_OK, "two-frame block accepted");
    check.expect(job->write(pcm + 4, 1) == AMBE_OK, "one-frame block accepted");
    check.expect(job->finish(&result) == AMBE_OK, "exact three-frame finish succeeds");
    check.expect(result.acceptedFrames == 3 && result.encodedFrames == 3, "finish counts are exact");
    check.expect(capture->samples == std::vector<float>(pcm, pcm + 6), "writer receives exact ordered samples");
    check.expect(job->abort() == AMBE_INVALID_STATE, "finished job cannot be canceled");
    check.expect(job->finish(&result) == AMBE_INVALID_STATE, "finish cannot repeat");

    capture = std::make_shared<Capture>(); job = makeJob(capture);
    check.expect(job->write(pcm, 2) == AMBE_OK, "short write accepted initially");
    check.expect(job->finish(&result) == AMBE_INVALID_STATE, "short finish fails");
    check.expect(job->state() == StreamingExportJob::State::Failed, "short finish marks job failed");
    check.expect(job->abort() == AMBE_OK && job->abort() == AMBE_OK, "abort failed job and repeat abort");
    check.expect(capture->closed, "abort closes writer");

    capture = std::make_shared<Capture>(); job = makeJob(capture);
    check.expect(job->write(pcm, 4) == AMBE_INVALID_ARGUMENT, "overrun block rejected");
    check.expect(capture->samples.empty(), "overrun block not partially written");
    check.expect(job->state() == StreamingExportJob::State::Failed, "overrun fails job");

    capture = std::make_shared<Capture>(); job = makeJob(capture);
    check.expect(job->write(pcm, 0) == AMBE_INVALID_ARGUMENT, "zero-frame block rejected");
    check.expect(job->state() == StreamingExportJob::State::Failed, "zero block fails job");

    capture = std::make_shared<Capture>(); job = makeJob(capture);
    const float nonfinite[] = {0.1f, std::numeric_limits<float>::quiet_NaN()};
    check.expect(job->write(nonfinite, 1) == AMBE_INVALID_ARGUMENT, "nonfinite PCM block rejected");
    check.expect(capture->samples.empty() && job->state() == StreamingExportJob::State::Failed,
        "nonfinite block never reaches writer");

    capture = std::make_shared<Capture>(); job = makeJob(capture);
    capture->throwWrite = true;
    check.expect(job->write(pcm, 1) == AMBE_INTERNAL_ERROR, "writer exception maps to internal error");
    check.expect(job->state() == StreamingExportJob::State::Failed, "writer exception fails job");

    capture = std::make_shared<Capture>(); job = makeJob(capture);
    capture->throwFinish = true;
    check.expect(job->write(pcm, 3) == AMBE_OK, "pre-finalize block accepted");
    check.expect(job->finish(&result) == AMBE_INTERNAL_ERROR, "finish exception maps to internal error");
    check.expect(job->state() == StreamingExportJob::State::Failed, "finish exception fails job");

    for (int malformed = 0; malformed < 3; ++malformed) {
        capture = std::make_shared<Capture>(); job = makeJob(capture);
        check.expect(job->write(pcm, 3) == AMBE_OK, "complete job before malformed result");
        AMBE_ResultV1 bad = result;
        if (malformed == 0) bad.structSize = 0;
        if (malformed == 1) bad.abiVersion = 2;
        if (malformed == 2) bad.reserved[0] = 1;
        check.expect(job->finish(&bad) == AMBE_INVALID_ARGUMENT, "malformed result refused");
        check.expect(job->state() == StreamingExportJob::State::Writing && !capture->closed,
            "malformed result does not finalize or fail job");
        check.expect(job->finish(&result) == AMBE_OK, "valid result can still finish");
    }

    const uint32_t maxStereoFrames = AMBE_MAX_BLOCK_BYTES / (2u * sizeof(float));
    std::vector<float> maxBlock(static_cast<size_t>(maxStereoFrames) * 2u, 0.25f);
    auto largeInput = stereo3(); largeInput.expectedFrames = maxStereoFrames;
    capture = std::make_shared<Capture>();
    job = std::make_shared<StreamingExportJob>(largeInput, std::make_unique<FakeWriter>(capture));
    check.expect(job->write(maxBlock.data(), maxStereoFrames) == AMBE_OK, "exact 1 MiB block accepted");
    check.expect(job->finish(&result) == AMBE_OK && result.encodedFrames == maxStereoFrames,
        "exact 1 MiB block finishes");
    largeInput.expectedFrames = static_cast<uint64_t>(maxStereoFrames) + 1;
    capture = std::make_shared<Capture>();
    job = std::make_shared<StreamingExportJob>(largeInput, std::make_unique<FakeWriter>(capture));
    std::vector<float> overBlock(static_cast<size_t>(maxStereoFrames + 1) * 2u, 0.25f);
    check.expect(job->write(overBlock.data(), maxStereoFrames + 1) == AMBE_INVALID_ARGUMENT,
        "one frame above 1 MiB block rejected even within expected total");
    check.expect(capture->samples.empty(), "oversize block writes nothing");

    ExportRegistry registry;
    capture = std::make_shared<Capture>();
    const AMBE_Handle token = registry.add(makeJob(capture));
    check.expect(token != 0 && registry.find(token) != nullptr, "registry stores nonzero token");
    check.expect(registry.destroy(token) == AMBE_OK, "destroy retires token");
    check.expect(capture->closed, "destroy closes unfinished writer");
    check.expect(registry.destroy(token) == AMBE_INVALID_STATE && !registry.find(token), "double destroy and stale token invalid");
    check.expect(registry.destroy(0) == AMBE_INVALID_STATE, "zero token invalid");
    return check.ok ? 0 : 1;
}
