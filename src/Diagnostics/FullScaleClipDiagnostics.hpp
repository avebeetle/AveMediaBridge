#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

namespace AveMediaBridge::Diagnostics {

constexpr double kFullScaleNearEpsilon = 0.000001;

struct FullScaleSampleStats {
    std::uint64_t sampleCount = 0;
    std::uint64_t finiteCount = 0;
    std::uint64_t nonFiniteCount = 0;
    double minValue = std::numeric_limits<double>::infinity();
    double maxValue = -std::numeric_limits<double>::infinity();
    double maxAbs = 0.0;
    std::uint64_t exactPositiveOneCount = 0;
    std::uint64_t exactNegativeOneCount = 0;
    std::uint64_t nearPositiveOneCount = 0;
    std::uint64_t nearNegativeOneCount = 0;
    std::uint64_t abovePositiveOneCount = 0;
    std::uint64_t belowNegativeOneCount = 0;
    std::uint64_t zeroCount = 0;
    std::string sampleFormatName;
    bool planar = false;
    bool planarMixed = false;
    int channels = 0;
    std::uint64_t frames = 0;
    bool supported = true;
    std::string unsupportedReason;
};

inline std::string sampleFormatName(AVSampleFormat format) {
    const char* name = av_get_sample_fmt_name(format);
    return name ? name : "unknown";
}

inline std::uint64_t totalSampleCount(int frames, int channels) {
    if (frames <= 0 || channels <= 0) {
        return 0;
    }
    const auto frameCount = static_cast<std::uint64_t>(frames);
    const auto channelCount = static_cast<std::uint64_t>(channels);
    if (frameCount > (std::numeric_limits<std::uint64_t>::max)() / channelCount) {
        return (std::numeric_limits<std::uint64_t>::max)();
    }
    return frameCount * channelCount;
}

inline int frameChannelCount(const AVFrame* frame, int fallbackChannels) {
    if (frame && frame->ch_layout.nb_channels > 0) {
        return frame->ch_layout.nb_channels;
    }
    return fallbackChannels;
}

inline void setStatsMetadata(
    FullScaleSampleStats& stats,
    AVSampleFormat format,
    bool planar,
    int channels,
    int frames) {
    stats.sampleFormatName = sampleFormatName(format);
    stats.planar = planar;
    stats.channels = channels;
    stats.frames = frames > 0 ? static_cast<std::uint64_t>(frames) : 0;
}

inline void markUnsupported(
    FullScaleSampleStats& stats,
    AVSampleFormat format,
    bool planar,
    int channels,
    int frames,
    std::string reason) {
    setStatsMetadata(stats, format, planar, channels, frames);
    stats.supported = false;
    stats.unsupportedReason = std::move(reason);
    stats.sampleCount = totalSampleCount(frames, channels);
}

inline void addSampleValue(FullScaleSampleStats& stats, double value) {
    ++stats.sampleCount;
    if (!std::isfinite(value)) {
        ++stats.nonFiniteCount;
        return;
    }

    ++stats.finiteCount;
    stats.minValue = (std::min)(stats.minValue, value);
    stats.maxValue = (std::max)(stats.maxValue, value);
    stats.maxAbs = (std::max)(stats.maxAbs, std::abs(value));

    if (value == 1.0) {
        ++stats.exactPositiveOneCount;
    }
    if (value == -1.0) {
        ++stats.exactNegativeOneCount;
    }
    if (std::abs(value - 1.0) <= kFullScaleNearEpsilon) {
        ++stats.nearPositiveOneCount;
    }
    if (std::abs(value + 1.0) <= kFullScaleNearEpsilon) {
        ++stats.nearNegativeOneCount;
    }
    if (value > 1.0) {
        ++stats.abovePositiveOneCount;
    }
    if (value < -1.0) {
        ++stats.belowNegativeOneCount;
    }
    if (value == 0.0) {
        ++stats.zeroCount;
    }
}

inline void mergeSampleStats(FullScaleSampleStats& aggregate, const FullScaleSampleStats& chunk) {
    if (aggregate.sampleFormatName.empty()) {
        aggregate.sampleFormatName = chunk.sampleFormatName;
        aggregate.planar = chunk.planar;
        aggregate.channels = chunk.channels;
    } else {
        if (aggregate.sampleFormatName != chunk.sampleFormatName) {
            aggregate.sampleFormatName = "mixed";
        }
        if (aggregate.planar != chunk.planar) {
            aggregate.planarMixed = true;
        }
        if (aggregate.channels != chunk.channels) {
            aggregate.channels = -1;
        }
    }

    const bool hadFinite = aggregate.finiteCount > 0;
    aggregate.sampleCount += chunk.sampleCount;
    aggregate.finiteCount += chunk.finiteCount;
    aggregate.nonFiniteCount += chunk.nonFiniteCount;
    aggregate.frames += chunk.frames;
    if (chunk.finiteCount > 0) {
        aggregate.minValue = hadFinite ? (std::min)(aggregate.minValue, chunk.minValue) : chunk.minValue;
        aggregate.maxValue = hadFinite ? (std::max)(aggregate.maxValue, chunk.maxValue) : chunk.maxValue;
        aggregate.maxAbs = (std::max)(aggregate.maxAbs, chunk.maxAbs);
    }
    aggregate.exactPositiveOneCount += chunk.exactPositiveOneCount;
    aggregate.exactNegativeOneCount += chunk.exactNegativeOneCount;
    aggregate.nearPositiveOneCount += chunk.nearPositiveOneCount;
    aggregate.nearNegativeOneCount += chunk.nearNegativeOneCount;
    aggregate.abovePositiveOneCount += chunk.abovePositiveOneCount;
    aggregate.belowNegativeOneCount += chunk.belowNegativeOneCount;
    aggregate.zeroCount += chunk.zeroCount;
    aggregate.supported = aggregate.supported && chunk.supported;
    if (!chunk.supported && aggregate.unsupportedReason.empty()) {
        aggregate.unsupportedReason = chunk.unsupportedReason;
    }
}

template <typename RawSample, typename Normalize>
inline void analyzePackedSamples(
    const AVFrame* frame,
    int channels,
    Normalize normalize,
    FullScaleSampleStats& stats) {
    const auto* samples = reinterpret_cast<const RawSample*>(frame->extended_data[0]);
    const std::uint64_t count = totalSampleCount(frame->nb_samples, channels);
    for (std::uint64_t i = 0; i < count; ++i) {
        addSampleValue(stats, normalize(samples[i]));
    }
}

template <typename RawSample, typename Normalize>
inline void analyzePlanarSamples(
    const AVFrame* frame,
    int channels,
    Normalize normalize,
    FullScaleSampleStats& stats) {
    for (int channel = 0; channel < channels; ++channel) {
        const auto* samples = reinterpret_cast<const RawSample*>(frame->extended_data[channel]);
        for (int frameIndex = 0; frameIndex < frame->nb_samples; ++frameIndex) {
            addSampleValue(stats, normalize(samples[frameIndex]));
        }
    }
}

template <typename RawSample, typename Normalize>
inline bool analyzeFrameWithSampleType(
    const AVFrame* frame,
    bool planar,
    int channels,
    Normalize normalize,
    FullScaleSampleStats& stats) {
    if (planar) {
        for (int channel = 0; channel < channels; ++channel) {
            if (!frame->extended_data[channel]) {
                return false;
            }
        }
        analyzePlanarSamples<RawSample>(frame, channels, normalize, stats);
        return true;
    }

    if (!frame->extended_data[0]) {
        return false;
    }
    analyzePackedSamples<RawSample>(frame, channels, normalize, stats);
    return true;
}

inline bool analyzeFrameSamples(
    const AVFrame* frame,
    int fallbackChannels,
    FullScaleSampleStats& stats) {
    if (!frame) {
        markUnsupported(stats, AV_SAMPLE_FMT_NONE, false, 0, 0, "null AVFrame");
        return false;
    }

    const auto format = static_cast<AVSampleFormat>(frame->format);
    const bool planar = av_sample_fmt_is_planar(format) != 0;
    const int channels = frameChannelCount(frame, fallbackChannels);
    setStatsMetadata(stats, format, planar, channels, frame->nb_samples);

    if (format == AV_SAMPLE_FMT_NONE) {
        markUnsupported(stats, format, planar, channels, frame->nb_samples, "unknown sample format");
        return false;
    }
    if (channels <= 0 || frame->nb_samples <= 0 || !frame->extended_data) {
        markUnsupported(stats, format, planar, channels, frame->nb_samples, "invalid frame shape");
        return false;
    }

    const AVSampleFormat packedFormat = av_get_packed_sample_fmt(format);
    bool ok = false;
    switch (packedFormat) {
    case AV_SAMPLE_FMT_U8:
        ok = analyzeFrameWithSampleType<std::uint8_t>(
            frame,
            planar,
            channels,
            [](std::uint8_t value) { return (static_cast<double>(value) - 128.0) / 128.0; },
            stats);
        break;
    case AV_SAMPLE_FMT_S16:
        ok = analyzeFrameWithSampleType<std::int16_t>(
            frame,
            planar,
            channels,
            [](std::int16_t value) { return static_cast<double>(value) / 32768.0; },
            stats);
        break;
    case AV_SAMPLE_FMT_S32:
        ok = analyzeFrameWithSampleType<std::int32_t>(
            frame,
            planar,
            channels,
            [](std::int32_t value) { return static_cast<double>(value) / 2147483648.0; },
            stats);
        break;
    case AV_SAMPLE_FMT_FLT:
        ok = analyzeFrameWithSampleType<float>(
            frame,
            planar,
            channels,
            [](float value) { return static_cast<double>(value); },
            stats);
        break;
    case AV_SAMPLE_FMT_DBL:
        ok = analyzeFrameWithSampleType<double>(
            frame,
            planar,
            channels,
            [](double value) { return value; },
            stats);
        break;
    default:
        markUnsupported(
            stats,
            format,
            planar,
            channels,
            frame->nb_samples,
            "unsupported sample format");
        return false;
    }

    if (!ok) {
        stats = {};
        markUnsupported(stats, format, planar, channels, frame->nb_samples, "missing sample data");
        return false;
    }
    return true;
}

inline bool analyzeInterleavedFloatSamples(
    const float* samples,
    int frames,
    int channels,
    FullScaleSampleStats& stats) {
    setStatsMetadata(stats, AV_SAMPLE_FMT_FLT, false, channels, frames);
    if (!samples || frames <= 0 || channels <= 0) {
        markUnsupported(stats, AV_SAMPLE_FMT_FLT, false, channels, frames, "invalid float buffer");
        return false;
    }

    const std::uint64_t count = totalSampleCount(frames, channels);
    for (std::uint64_t i = 0; i < count; ++i) {
        addSampleValue(stats, static_cast<double>(samples[i]));
    }
    return true;
}

}  // namespace AveMediaBridge::Diagnostics
