#include "Export/StreamingExportJob.hpp"

#include <chrono>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace AveMediaBridge::Export;
using namespace std::chrono_literals;

struct Check {
    unsigned checks = 0;
    unsigned failures = 0;
    void expect(bool condition, const char* message) {
        ++checks;
        if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
    }
};

struct WriterGate {
    std::promise<void> entered;
    std::shared_future<void> resume;

    void hold() {
        entered.set_value();
        resume.wait();
    }
};

struct Capture {
    std::vector<float> samples;
    bool closed = false;
    bool throwWrite = false;
    bool throwFinish = false;
    uint64_t encodedOverride = 0;
    WriterGate* writeGate = nullptr;
    WriterGate* finishGate = nullptr;
    unsigned aborts = 0;
};

class FakeWriter final : public IStreamingPcmWriter {
public:
    explicit FakeWriter(std::shared_ptr<Capture> capture) : capture_(std::move(capture)) {}
    void write(const float* samples, uint32_t frames) override {
        if (capture_->writeGate) capture_->writeGate->hold();
        if (capture_->throwWrite) throw std::runtime_error("write");
        capture_->samples.insert(capture_->samples.end(), samples, samples + frames * 2);
    }
    uint64_t finish() override {
        if (capture_->finishGate) capture_->finishGate->hold();
        if (capture_->throwFinish) throw std::runtime_error("finish");
        capture_->closed = true;
        return capture_->encodedOverride ? capture_->encodedOverride : capture_->samples.size() / 2;
    }
    void abort() noexcept override {
        ++capture_->aborts;
        capture_->closed = true;
    }
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

static void checkDestroyDuringOperation(Check& check, bool finishing) {
    // A lookup completed before retirement pins the job. A writer call already
    // holding the job lock completes first. Destroy then aborts a writing job
    // or leaves a finished job alone. Lookups after retirement fail immediately.
    std::promise<void> releaseWriter;
    WriterGate gate{{}, releaseWriter.get_future().share()};
    auto entered = gate.entered.get_future();
    const float pcm[] = {0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f};
    ExportRegistry registry;
    auto capture = std::make_shared<Capture>();
    auto job = makeJob(capture);
    if (finishing) {
        check.expect(job->write(pcm, 3) == AMBE_OK, "finish overlap has complete input");
        capture->finishGate = &gate;
    } else {
        capture->writeGate = &gate;
    }
    const auto token = registry.add(job);
    const std::weak_ptr<StreamingExportJob> lifetime = job;
    job.reset();
    AMBE_ResultV1 result{sizeof(result), AMBE_ABI_VERSION};
    auto operation = std::async(std::launch::async, [&] {
        auto pin = registry.find(token);
        if (!pin) return AMBE_INVALID_STATE;
        return finishing ? pin->finish(&result) : pin->write(pcm, 3);
    });
    const bool held = entered.wait_for(5s) == std::future_status::ready;
    check.expect(held, "writer operation reached its gate");
    auto destroy = std::async(std::launch::async, [&] { return registry.destroy(token); });
    auto lookup = std::async(std::launch::async, [&] {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!registry.find(token)) return true;
            std::this_thread::yield();
        }
        return false;
    });
    const bool lookupReady = lookup.wait_for(5s) == std::future_status::ready;
    const bool retired = lookupReady && lookup.get();
    check.expect(retired, "destroy retires entry while writer is held");
    if (held && retired) {
        check.expect(!lifetime.expired(), "retired job remains live during the call");
        check.expect(destroy.wait_for(0s) == std::future_status::timeout,
            "destroy waits for the active job call");
        check.expect(registry.destroy(token) == AMBE_INVALID_STATE,
            "second destroy sees retired token during the call");
    }
    releaseWriter.set_value();
    if (!lookupReady) lookup.get();
    check.expect(operation.get() == AMBE_OK, "in-flight job operation completes");
    check.expect(destroy.get() == AMBE_OK, "first destroy completes after the call");
    check.expect(lifetime.expired(), "last pin releases the retired job");
    check.expect(capture->closed && capture->aborts == (finishing ? 0u : 1u),
        "destroy aborts writing once and preserves a completed finish");
    check.expect(capture->samples == std::vector<float>(pcm, pcm + 6),
        "in-flight writer reads caller PCM while it is live");
    if (finishing) {
        check.expect(result.acceptedFrames == 3 && result.encodedFrames == 3,
            "in-flight finish returns complete counts");
    }
    const auto next = registry.add(makeJob(std::make_shared<Capture>()));
    check.expect(next != 0 && next != token && !registry.find(token),
        "a new job never revives the stale token");
}

int main() {
    Check check;
    const float pcm[] = {0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f, 0.4f, -0.4f};
    AMBE_ResultV1 result{}; result.structSize = sizeof(result); result.abiVersion = AMBE_ABI_VERSION;

    auto input = stereo3();
    check.expect(validateInput(&input) == AMBE_OK, "valid export input");
    input.structSize--;
    check.expect(validateInput(&input) == AMBE_INVALID_ARGUMENT, "invalid export input size");
    input = stereo3(); input.abiVersion++;
    check.expect(validateInput(&input) == AMBE_INVALID_ARGUMENT, "invalid export input version");
    input = stereo3(); input.reserved[3] = 1;
    check.expect(validateInput(&input) == AMBE_INVALID_ARGUMENT, "nonzero export input reserved field");
    input = stereo3(); input.expectedFrames = UINT64_MAX;
    check.expect(validateInput(&input) == AMBE_INVALID_ARGUMENT, "frame extent overflow rejected");
    input = stereo3(); input.sampleRate = INT32_MAX;
    check.expect(validateInput(&input) == AMBE_INVALID_ARGUMENT, "byte rate overflow rejected");

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
    check.expect(capture->closed && capture->aborts == 1, "repeated abort closes writer once");

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
    checkDestroyDuringOperation(check, false);
    checkDestroyDuringOperation(check, true);
    std::cout << "checks=" << check.checks << " failures=" << check.failures << '\n';
    return check.failures ? 1 : 0;
}
