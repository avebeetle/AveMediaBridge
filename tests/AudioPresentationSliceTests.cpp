#include "../src/Decode/AudioPresentationSlice.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace {

using AveMediaBridge::Decode::DecodedAudioPresentationInput;
using AveMediaBridge::Decode::DecodedAudioPresentationReason;
using AveMediaBridge::Decode::resolvePresentedInputSamples;

bool expect(
    bool condition,
    const std::string& name,
    const std::string& detail = {}) {
    if (condition) {
        return true;
    }

    std::cerr << "audioPresentationSliceTest: case=\"" << name << "\" pass=no";
    if (!detail.empty()) {
        std::cerr << " detail=\"" << detail << "\"";
    }
    std::cerr << "\n";
    return false;
}

DecodedAudioPresentationInput baseInput() {
    DecodedAudioPresentationInput input;
    input.frameNbSamples = 1024;
    input.frameDurationKnown = true;
    input.frameDuration = 1024;
    input.frameDurationTimeBase = AVRational{1, 44100};
    input.inputSampleRate = 44100;
    return input;
}

bool checkPlan(
    const std::string& name,
    const DecodedAudioPresentationInput& input,
    int accepted,
    int trimmed,
    DecodedAudioPresentationReason reason) {
    const auto plan = resolvePresentedInputSamples(input);
    bool ok = true;
    ok &= expect(plan.physicalInputSamples == (std::max)(0, input.frameNbSamples), name);
    ok &= expect(plan.acceptedInputSamples == accepted, name);
    ok &= expect(plan.trimmedTailSamples == trimmed, name);
    ok &= expect(plan.reason == reason, name);
    return ok;
}

bool runPolicyFixtures() {
    bool ok = true;

    ok &= checkPlan(
        "full frame",
        baseInput(),
        1024,
        0,
        DecodedAudioPresentationReason::FullPhysicalFrame);

    auto shortDuration = baseInput();
    shortDuration.frameDuration = 384;
    ok &= checkPlan(
        "short frame duration",
        shortDuration,
        384,
        640,
        DecodedAudioPresentationReason::LimitedByFrameDuration);

    auto unknownDuration = baseInput();
    unknownDuration.frameDurationKnown = false;
    unknownDuration.frameDuration = 0;
    ok &= checkPlan(
        "unknown duration",
        unknownDuration,
        1024,
        0,
        DecodedAudioPresentationReason::FullPhysicalFrame);

    auto budgetShorter = baseInput();
    budgetShorter.remainingPresentationFramesKnown = true;
    budgetShorter.remainingPresentationFrames = 300;
    ok &= checkPlan(
        "budget shorter than duration",
        budgetShorter,
        300,
        724,
        DecodedAudioPresentationReason::LimitedByPresentationBudget);

    auto durationShorter = shortDuration;
    durationShorter.remainingPresentationFramesKnown = true;
    durationShorter.remainingPresentationFrames = 500;
    ok &= checkPlan(
        "duration shorter than budget",
        durationShorter,
        384,
        640,
        DecodedAudioPresentationReason::LimitedByFrameDuration);

    auto durationAndBudget = shortDuration;
    durationAndBudget.remainingPresentationFramesKnown = true;
    durationAndBudget.remainingPresentationFrames = 300;
    ok &= checkPlan(
        "duration and budget",
        durationAndBudget,
        300,
        724,
        DecodedAudioPresentationReason::LimitedByDurationAndBudget);

    auto invalidTimeBase = shortDuration;
    invalidTimeBase.frameDurationTimeBase = AVRational{0, 1};
    ok &= checkPlan(
        "invalid duration time base",
        invalidTimeBase,
        1024,
        0,
        DecodedAudioPresentationReason::UnknownDuration);

    auto nonExact = baseInput();
    nonExact.frameDuration = 1;
    nonExact.frameDurationTimeBase = AVRational{1, 1000};
    ok &= checkPlan(
        "non-exact rational duration conversion",
        nonExact,
        1024,
        0,
        DecodedAudioPresentationReason::UnknownDuration);

    auto zeroAccepted = baseInput();
    zeroAccepted.remainingPresentationFramesKnown = true;
    zeroAccepted.remainingPresentationFrames = 0;
    ok &= checkPlan(
        "zero accepted samples",
        zeroAccepted,
        0,
        1024,
        DecodedAudioPresentationReason::LimitedByPresentationBudget);

    auto overflowBudget = baseInput();
    overflowBudget.remainingPresentationFramesKnown = true;
    overflowBudget.remainingPresentationFrames =
        (std::numeric_limits<std::uint64_t>::max)();
    ok &= checkPlan(
        "overflow boundaries",
        overflowBudget,
        1024,
        0,
        DecodedAudioPresentationReason::FullPhysicalFrame);

    auto durationGreater = baseInput();
    durationGreater.frameDuration = 2048;
    ok &= checkPlan(
        "duration greater than nb_samples",
        durationGreater,
        1024,
        0,
        DecodedAudioPresentationReason::FullPhysicalFrame);

    DecodedAudioPresentationInput invalidFrame;
    invalidFrame.frameNbSamples = -1;
    ok &= checkPlan(
        "invalid frame",
        invalidFrame,
        0,
        0,
        DecodedAudioPresentationReason::InvalidFrame);

    return ok;
}

bool runSwrTrimSmoke() {
    DecodedAudioPresentationInput input;
    input.frameNbSamples = 1024;
    input.frameDurationKnown = true;
    input.frameDuration = 480;
    input.frameDurationTimeBase = AVRational{1, 48000};
    input.inputSampleRate = 48000;
    const auto plan = resolvePresentedInputSamples(input);
    if (!expect(plan.acceptedInputSamples == 480, "swr trim smoke")) {
        return false;
    }

    AVChannelLayout mono{};
    av_channel_layout_default(&mono, 1);
    SwrContext* swr = nullptr;
    int ret = swr_alloc_set_opts2(
        &swr,
        &mono,
        AV_SAMPLE_FMT_FLT,
        44100,
        &mono,
        AV_SAMPLE_FMT_FLT,
        48000,
        0,
        nullptr);
    if (!expect(ret >= 0 && swr != nullptr, "swr alloc")) {
        av_channel_layout_uninit(&mono);
        return false;
    }
    ret = swr_init(swr);
    if (!expect(ret >= 0, "swr init")) {
        swr_free(&swr);
        av_channel_layout_uninit(&mono);
        return false;
    }

    std::vector<float> inputSamples(1024, 1000.0f);
    for (int i = 0; i < plan.acceptedInputSamples; ++i) {
        inputSamples[static_cast<std::size_t>(i)] = 0.25f;
    }

    const int outCapacity = swr_get_out_samples(swr, plan.acceptedInputSamples);
    std::vector<float> output(static_cast<std::size_t>((std::max)(1, outCapacity)), 0.0f);
    const uint8_t* inData[] = { reinterpret_cast<const uint8_t*>(inputSamples.data()) };
    uint8_t* outData[] = { reinterpret_cast<uint8_t*>(output.data()) };
    ret = swr_convert(swr, outData, outCapacity, inData, plan.acceptedInputSamples);
    if (!expect(ret >= 0, "swr convert")) {
        swr_free(&swr);
        av_channel_layout_uninit(&mono);
        return false;
    }

    float maxAbs = 0.0f;
    for (int i = 0; i < ret; ++i) {
        maxAbs = (std::max)(maxAbs, std::fabs(output[static_cast<std::size_t>(i)]));
    }

    while (swr_get_delay(swr, 48000) > 0) {
        const int flushCapacity = swr_get_out_samples(swr, 0);
        if (flushCapacity <= 0) {
            break;
        }
        output.assign(static_cast<std::size_t>(flushCapacity), 0.0f);
        uint8_t* flushData[] = { reinterpret_cast<uint8_t*>(output.data()) };
        const int flushed = swr_convert(swr, flushData, flushCapacity, nullptr, 0);
        if (!expect(flushed >= 0, "swr flush")) {
            swr_free(&swr);
            av_channel_layout_uninit(&mono);
            return false;
        }
        if (flushed == 0) {
            break;
        }
        for (int i = 0; i < flushed; ++i) {
            maxAbs = (std::max)(maxAbs, std::fabs(output[static_cast<std::size_t>(i)]));
        }
    }

    swr_free(&swr);
    av_channel_layout_uninit(&mono);

    return expect(maxAbs < 10.0f, "swr trim smoke", "padding sentinel reached output");
}

}  // namespace

int main() {
    if (!runPolicyFixtures()) {
        return 1;
    }
    if (!runSwrTrimSmoke()) {
        return 1;
    }

    std::cout << "AVEMEDIABRIDGE_AUDIO_PRESENTATION_SLICE_OK\n";
    return 0;
}
