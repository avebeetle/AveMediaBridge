#pragma once

#include "../Ffmpeg/FfmpegHeaders.hpp"

#include <cstdint>

namespace AveMediaBridge::Decode {

enum class DecodedAudioPresentationReason {
    FullPhysicalFrame,
    LimitedByFrameDuration,
    LimitedByPresentationBudget,
    LimitedByDurationAndBudget,
    InvalidFrame,
    UnknownDuration
};

struct DecodedAudioPresentationInput {
    int frameNbSamples = 0;

    bool frameDurationKnown = false;
    std::int64_t frameDuration = 0;
    AVRational frameDurationTimeBase{0, 1};

    int inputSampleRate = 0;

    bool remainingPresentationFramesKnown = false;
    std::uint64_t remainingPresentationFrames = 0;
};

struct DecodedAudioPresentationPlan {
    int physicalInputSamples = 0;
    int acceptedInputSamples = 0;
    int trimmedTailSamples = 0;

    bool frameDurationSampleExact = false;
    int frameDurationSamples = 0;
    bool durationLimitApplied = false;
    bool presentationBudgetApplied = false;

    DecodedAudioPresentationReason reason =
        DecodedAudioPresentationReason::FullPhysicalFrame;
};

AVRational resolveDecodedFrameDurationTimeBase(
    const AVFrame* frame,
    const AVCodecContext* decoder) noexcept;

DecodedAudioPresentationPlan resolvePresentedInputSamples(
    const DecodedAudioPresentationInput& input) noexcept;

const char* decodedAudioPresentationReasonName(
    DecodedAudioPresentationReason reason) noexcept;

}  // namespace AveMediaBridge::Decode
