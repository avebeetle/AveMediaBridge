#pragma once
#include "../Input/SelectedAudioBinding.hpp"
#include "../Ffmpeg/FfmpegHeaders.hpp"
#include <string>

namespace AveMediaBridge::Probe {
struct ReaderMediaFacts {
    Input::SelectedAudioBinding selectedAudio;
    std::string videoClassification = "unknown";
    unsigned timedVideoCount = 0;
    int videoStreamIndex = -1;
    int videoTrackId = -1;
    int videoCodecId = 0;
    std::string videoCodec;
    std::string videoReason = "notQualified";
};
ReaderMediaFacts captureReaderMediaFacts(const AVFormatContext*, const Input::SelectedAudioBinding&);
std::string encodeReaderMediaFacts(const ReaderMediaFacts&);
}
