#pragma once

#include "Input/MediaInputSource.hpp"
#include <cstdint>
#include <vector>

namespace AveMediaBridge::Vorbis {
enum class Status { Ok, Invalid, UnsupportedChannels, QuickScanLimit, ExtendedLimit, IoError, Cancelled };
struct Link {
    int rate, channels;
    std::int64_t frames, output;
};
struct Plan {
    int rate = 0, channels = 0, unsupportedChannels = 0;
    std::int64_t frames = 0;
    bool ordinarySingleLink = false;
    bool singlePageMultichannel = false;
    std::vector<Link> parts;
};

// A plan is evidence for this source only. Keep the same source pin for decoding.
Status probe(const Input::MediaInputSource& source, Plan& result);
// Sequential, verified packet count for the immutable source after a quick probe limit.
Status measure(const Input::MediaInputSource& source, Plan& result);
using Emit = int (__cdecl *)(void* user, const float* audio, int frames, int channels,
    int rate, int link, std::uint64_t sourceBytes);
using Cancel = bool (*)(void* user);
Status decode(const Input::MediaInputSource& source, const Plan& plan, void* user,
    Emit emit, Cancel cancel, std::uint64_t& sourceBytes);
}
