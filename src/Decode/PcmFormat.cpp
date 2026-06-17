#include "PcmFormat.hpp"

namespace AveMediaBridge::Decode {

std::string sampleFormatName(AVSampleFormat format) {
    const char* name = av_get_sample_fmt_name(format);
    return name ? std::string(name) : std::string();
}

}  // namespace AveMediaBridge::Decode
