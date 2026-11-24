#pragma once

#include "PresentationBudgetPolicy.hpp"

namespace AveMediaBridge::Probe {
// Resolve fixed-width audio in a single, final RIFF data chunk on the open demuxer.
bool matchesRiffPcmExtent(const AVFormatContext*, const AVStream*, const TotalPresentationEvidence&);
}
