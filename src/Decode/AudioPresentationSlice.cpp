#include "AudioPresentationSlice.hpp"

#include <algorithm>
#include <limits>

namespace AveMediaBridge::Decode {
namespace {

bool validTimeBase(AVRational value) noexcept {
    return value.num > 0 && value.den > 0;
}

}  // namespace

AVRational resolveDecodedFrameDurationTimeBase(
    const AVFrame* frame,
    const AVCodecContext* decoder) noexcept {
    if (frame && validTimeBase(frame->time_base)) {
        return frame->time_base;
    }

    if (decoder && validTimeBase(decoder->pkt_timebase)) {
        return decoder->pkt_timebase;
    }

    return AVRational{0, 1};
}

DecodedAudioPresentationPlan resolvePresentedInputSamples(
    const DecodedAudioPresentationInput& input) noexcept {
    DecodedAudioPresentationPlan plan;
    plan.physicalInputSamples = (std::max)(0, input.frameNbSamples);

    if (input.frameNbSamples <= 0) {
        plan.reason = DecodedAudioPresentationReason::InvalidFrame;
        return plan;
    }

    int accepted = input.frameNbSamples;
    bool durationKnownButUnusable = false;

    // nb_samples is the physical decoder block size. A shorter positive
    // presentation duration limits the samples that belong to the media timeline.
    if (input.frameDurationKnown) {
        if (input.frameDuration > 0 &&
            input.inputSampleRate > 0 &&
            validTimeBase(input.frameDurationTimeBase)) {
            const AVRational sampleTimeBase{1, input.inputSampleRate};
            const std::int64_t candidateSamples =
                av_rescale_q(input.frameDuration, input.frameDurationTimeBase, sampleTimeBase);
            const bool sampleExact =
                candidateSamples > 0 &&
                av_compare_ts(
                    input.frameDuration,
                    input.frameDurationTimeBase,
                    candidateSamples,
                    sampleTimeBase) == 0;

            if (sampleExact &&
                candidateSamples <=
                    static_cast<std::int64_t>((std::numeric_limits<int>::max)())) {
                plan.frameDurationSampleExact = true;
                plan.frameDurationSamples = static_cast<int>(candidateSamples);
                if (plan.frameDurationSamples > 0 &&
                    plan.frameDurationSamples < input.frameNbSamples) {
                    accepted = plan.frameDurationSamples;
                    plan.durationLimitApplied = true;
                }
            } else {
                durationKnownButUnusable = true;
            }
        } else {
            durationKnownButUnusable = true;
        }
    }

    if (input.remainingPresentationFramesKnown) {
        const int budgetSamples =
            input.remainingPresentationFrames >
                    static_cast<std::uint64_t>((std::numeric_limits<int>::max)())
                ? (std::numeric_limits<int>::max)()
                : static_cast<int>(input.remainingPresentationFrames);
        if (budgetSamples < accepted) {
            accepted = (std::max)(0, budgetSamples);
            plan.presentationBudgetApplied = true;
        }
    }

    plan.acceptedInputSamples = accepted;
    plan.trimmedTailSamples = input.frameNbSamples - accepted;

    if (plan.durationLimitApplied && plan.presentationBudgetApplied) {
        plan.reason = DecodedAudioPresentationReason::LimitedByDurationAndBudget;
    } else if (plan.durationLimitApplied) {
        plan.reason = DecodedAudioPresentationReason::LimitedByFrameDuration;
    } else if (plan.presentationBudgetApplied) {
        plan.reason = DecodedAudioPresentationReason::LimitedByPresentationBudget;
    } else if (durationKnownButUnusable) {
        plan.reason = DecodedAudioPresentationReason::UnknownDuration;
    } else {
        plan.reason = DecodedAudioPresentationReason::FullPhysicalFrame;
    }

    return plan;
}

const char* decodedAudioPresentationReasonName(
    DecodedAudioPresentationReason reason) noexcept {
    switch (reason) {
        case DecodedAudioPresentationReason::FullPhysicalFrame:
            return "FullPhysicalFrame";
        case DecodedAudioPresentationReason::LimitedByFrameDuration:
            return "LimitedByFrameDuration";
        case DecodedAudioPresentationReason::LimitedByPresentationBudget:
            return "LimitedByPresentationBudget";
        case DecodedAudioPresentationReason::LimitedByDurationAndBudget:
            return "LimitedByDurationAndBudget";
        case DecodedAudioPresentationReason::InvalidFrame:
            return "InvalidFrame";
        case DecodedAudioPresentationReason::UnknownDuration:
            return "UnknownDuration";
    }
    return "Unknown";
}

}  // namespace AveMediaBridge::Decode
