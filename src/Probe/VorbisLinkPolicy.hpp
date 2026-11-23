#pragma once

#include "../Decode/VorbisPresentation.hpp"
#include "../Input/DemuxByteSource.hpp"
#include "PresentationBudgetPolicy.hpp"

namespace AveMediaBridge::Probe {
inline bool isStandaloneVorbis(const AVFormatContext* demux, const AVStream* audio) {
    return demux && demux->iformat && demux->iformat->name && demux->nb_streams == 1 &&
        std::strcmp(demux->iformat->name, "ogg") == 0 && audio && audio->codecpar &&
        audio->codecpar->codec_id == AV_CODEC_ID_VORBIS;
}

inline Vorbis::Status probeVorbisLinks(AVIOContext* io, Vorbis::Plan& plan) {
    Input::DemuxByteSource input(io);
    if (!input.valid()) return Vorbis::Status::IoError;
    const auto status = Vorbis::probe(input.source({}), plan);
    return input.restore() ? status : Vorbis::Status::IoError;
}

inline bool nativeVorbisShape(const Vorbis::Plan& plan, const AVStream* stream) {
    return stream && stream->codecpar &&
        stream->codecpar->sample_rate == plan.rate &&
        stream->codecpar->ch_layout.nb_channels == plan.channels;
}

inline bool nativeVorbisPresentation(const Vorbis::Plan& plan, const AVStream* stream,
                                     const TotalPresentationEvidence& native) {
    return plan.ordinarySingleLink && nativeVorbisShape(plan, stream) &&
        native.trust == PresentationTotalTrust::SampleExact &&
        native.frames == static_cast<std::uint64_t>(plan.frames);
}

inline bool nativeVorbisSinglePage(Vorbis::Status status, const Vorbis::Plan& plan, const AVStream* stream) {
    return status == Vorbis::Status::UnsupportedChannels && plan.singlePageMultichannel &&
        nativeVorbisShape(plan, stream);
}

inline TotalPresentationEvidence vorbisLinkEvidence(const Vorbis::Plan& plan) {
    TotalPresentationEvidence result;
    result.frames = static_cast<std::uint64_t>(plan.frames);
    result.trust = PresentationTotalTrust::SampleExact;
    result.source = PresentationTotalSource::OggVorbisLinkPresentation;
    result.domain = PresentationSampleDomain::DocumentSamples;
    result.sampleRate = plan.rate;
    result.exactRescale = true;
    result.validation = PresentationTotalValidation::SelfContainedMetadata;
    return result;
}

inline const char* vorbisFailureText(Vorbis::Status status) {
    switch (status) {
    case Vorbis::Status::QuickScanLimit: return "Could not confirm the audio length within the quick scan limit.";
    case Vorbis::Status::UnsupportedChannels: return "Vorbis import supports mono and stereo audio.";
    case Vorbis::Status::IoError: return "Could not read the Vorbis audio.";
    case Vorbis::Status::Cancelled: return "Vorbis import canceled.";
    default: return "The Vorbis audio structure is invalid or truncated.";
    }
}
} // namespace AveMediaBridge::Probe
