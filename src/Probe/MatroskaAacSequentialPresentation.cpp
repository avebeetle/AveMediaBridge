
#include "MatroskaAacSequentialPresentation.hpp"

#include "AacAudioSpecificConfig.hpp"
#include "EbmlSequentialReader.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <optional>
#include <string_view>

namespace AveMediaBridge::Probe {

using MatroskaAacDetail::AacConfig;
using MatroskaAacDetail::EbmlElementHeader;
using MatroskaAacDetail::EbmlVint;
using MatroskaAacDetail::ForwardFileReader;
using MatroskaAacDetail::checkedAdd;
using MatroskaAacDetail::parseAacAudioSpecificConfig;
using MatroskaAacDetail::readEbmlElementHeader;
using MatroskaAacDetail::readEbmlVint;
using MatroskaAacDetail::readMemoryVint;
using MatroskaAacDetail::readSigned;
using MatroskaAacDetail::readUnsigned;
using MatroskaAacDetail::validateEbmlCrc32;

struct SequentialSelectedStream {
    int streamIndex = -1;
    int audioOrdinal = -1;
    std::uint64_t trackNumber = 0;
    int sampleRate = 0;
    int channels = 0;
    int initialPadding = 0;
    std::vector<std::uint8_t> codecPrivate;
};

struct SequentialTrackInfo {
    std::uint64_t trackNumber = 0;
    std::uint64_t trackUid = 0;
    std::uint64_t trackType = 0;
    std::string codecId;
    std::vector<std::uint8_t> codecPrivate;
    std::uint64_t codecDelayNs = 0;
    std::uint64_t seekPreRollNs = 0;
    std::uint64_t defaultDurationNs = 0;
    double trackTimestampScale = 1.0;
    double samplingFrequency = 0.0;
    double outputSamplingFrequency = 0.0;
    std::uint64_t channels = 0;
};

struct SequentialScanState {
    std::string status = "unsupported";
    std::string reason = "not_scanned";
    std::uint64_t fileSizeBytes = 0;
    std::uint64_t bytesReturned = 0;
    std::uint64_t uniqueBytes = 0;
    std::uint64_t duplicateBytes = 0;
    std::uint64_t readCalls = 0;
    std::uint64_t seekCallsAfterOpen = 0;
    std::uint64_t maximumWorkingBytes = 0;
    std::uint64_t elementsParsed = 0;
    std::uint64_t clustersParsed = 0;
    std::uint64_t blocksParsed = 0;
    std::uint64_t selectedBlocks = 0;
    std::uint64_t selectedAccessUnits = 0;
    std::uint64_t lacedSelectedBlocks = 0;
    std::uint64_t selectedTrackNumber = 0;
    std::uint64_t selectedTrackUid = 0;
    std::uint64_t timestampScaleNs = 1000000;
    std::uint64_t codecDelayNs = 0;
    std::uint64_t initialSkipFrames = 0;
    std::uint64_t terminalDiscardFrames = 0;
    std::uint64_t physicalFrames = 0;
    std::uint64_t presentationFrames = 0;
    std::int64_t firstSelectedTimestampNs = 0;
    std::int64_t lastSelectedTimestampNs = 0;
    std::int64_t lastDiscardPaddingNs = 0;
    double segmentDurationTicks = 0.0;
    double scanWallMs = 0.0;
    double threadCpuMs = 0.0;
    bool segmentSizeUnknown = false;
    bool clusterSizeUnknown = false;
    bool reachedPhysicalEof = false;
    bool reachedSegmentEnd = false;
    bool mappingProven = false;
    bool allLacingValid = true;
    bool selectedTimestampContinuity = true;
    bool discardPaddingPresent = false;
    bool terminalZeroProvenByCompleteAbsence = false;
    bool crcElementPresent = false;
    bool cuesPresent = false;
    bool seekHeadPresent = false;
    AacConfig aac;
    std::vector<SequentialTrackInfo> tracks;
};

namespace {

constexpr std::uint64_t ID_EBML = 0x1A45DFA3;
constexpr std::uint64_t ID_DOC_TYPE = 0x4282;
constexpr std::uint64_t ID_SEGMENT = 0x18538067;
constexpr std::uint64_t ID_SEEK_HEAD = 0x114D9B74;
constexpr std::uint64_t ID_INFO = 0x1549A966;
constexpr std::uint64_t ID_TIMESTAMP_SCALE = 0x2AD7B1;
constexpr std::uint64_t ID_DURATION = 0x4489;
constexpr std::uint64_t ID_TRACKS = 0x1654AE6B;
constexpr std::uint64_t ID_TRACK_ENTRY = 0xAE;
constexpr std::uint64_t ID_TRACK_NUMBER = 0xD7;
constexpr std::uint64_t ID_TRACK_UID = 0x73C5;
constexpr std::uint64_t ID_TRACK_TYPE = 0x83;
constexpr std::uint64_t ID_CODEC_ID = 0x86;
constexpr std::uint64_t ID_CODEC_PRIVATE = 0x63A2;
constexpr std::uint64_t ID_DEFAULT_DURATION = 0x23E383;
constexpr std::uint64_t ID_CODEC_DELAY = 0x56AA;
constexpr std::uint64_t ID_SEEK_PREROLL = 0x56BB;
constexpr std::uint64_t ID_TRACK_TIMESTAMP_SCALE = 0x23314F;
constexpr std::uint64_t ID_AUDIO = 0xE1;
constexpr std::uint64_t ID_SAMPLING_FREQUENCY = 0xB5;
constexpr std::uint64_t ID_OUTPUT_SAMPLING_FREQUENCY = 0x78B5;
constexpr std::uint64_t ID_CHANNELS = 0x9F;
constexpr std::uint64_t ID_CLUSTER = 0x1F43B675;
constexpr std::uint64_t ID_CLUSTER_TIMESTAMP = 0xE7;
constexpr std::uint64_t ID_SIMPLE_BLOCK = 0xA3;
constexpr std::uint64_t ID_BLOCK_GROUP = 0xA0;
constexpr std::uint64_t ID_BLOCK = 0xA1;
constexpr std::uint64_t ID_BLOCK_DURATION = 0x9B;
constexpr std::uint64_t ID_DISCARD_PADDING = 0x75A2;
constexpr std::uint64_t ID_CUES = 0x1C53BB6B;
constexpr std::uint64_t ID_VOID = 0xEC;
constexpr std::uint64_t ID_CRC32 = 0xBF;
constexpr std::uint64_t MAX_SMALL_MASTER = 4U * 1024U * 1024U;
constexpr std::uint64_t MAX_BLOCK_BYTES = 16U * 1024U * 1024U;
constexpr std::uint64_t MAX_LACING_HEADER_BYTES = 64U * 1024U;

using Clock = std::chrono::steady_clock;

std::uint32_t readCrc32Value(const std::uint8_t data[4]) noexcept {
    return static_cast<std::uint32_t>(data[0]) |
        static_cast<std::uint32_t>(data[1]) << 8 |
        static_cast<std::uint32_t>(data[2]) << 16 |
        static_cast<std::uint32_t>(data[3]) << 24;
}

double threadCpuMs() noexcept {
    FILETIME create{}, exit{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &create, &exit, &kernel, &user)) {
        return 0.0;
    }
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return static_cast<double>(k.QuadPart + u.QuadPart) / 10000.0;
}

bool payloadEnd(
    const EbmlElementHeader& header,
    std::uint64_t parentEnd,
    std::uint64_t& end) noexcept {
    if (header.unknownSize || !checkedAdd(header.payloadOffset, header.size, end)) {
        return false;
    }
    return end <= parentEnd;
}

struct MemoryElement {
    std::uint64_t id = 0;
    std::size_t offset = 0;
    std::size_t payload = 0;
    std::size_t end = 0;
};

bool nextMemoryElement(
    const std::vector<std::uint8_t>& data,
    std::size_t& cursor,
    std::size_t parentEnd,
    MemoryElement& element) noexcept {
    if (cursor >= parentEnd || parentEnd > data.size()) {
        return false;
    }
    const EbmlVint id = readMemoryVint(data.data(), parentEnd, cursor, true);
    const EbmlVint size = id.valid
        ? readMemoryVint(data.data(), parentEnd, cursor + id.length, false)
        : EbmlVint{};
    if (!id.valid || !size.valid || size.unknown) {
        return false;
    }
    const std::size_t payload = cursor + id.length + size.length;
    if (size.value > parentEnd - payload) {
        return false;
    }
    element.id = id.value;
    element.offset = cursor;
    element.payload = payload;
    element.end = payload + static_cast<std::size_t>(size.value);
    cursor = element.end;
    return true;
}

bool validateLeadingMemoryCrc(
    const std::vector<std::uint8_t>& data,
    const MemoryElement& element,
    std::size_t parentBegin,
    std::size_t parentEnd) noexcept {
    if (element.offset != parentBegin || element.end - element.payload != 4 ||
        element.end > parentEnd) {
        return false;
    }
    const auto expected = readCrc32Value(data.data() + element.payload);
    return validateEbmlCrc32(
        data.data() + element.end,
        parentEnd - element.end,
        expected);
}

double readFloat(const std::uint8_t* data, std::size_t size) noexcept {
    if (size == 4) {
        std::uint32_t bits = static_cast<std::uint32_t>(readUnsigned(data, size));
        float value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    if (size == 8) {
        std::uint64_t bits = readUnsigned(data, size);
        double value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    return 0.0;
}

bool parseAudio(
    const std::vector<std::uint8_t>& data,
    std::size_t begin,
    std::size_t end,
    SequentialTrackInfo& track) {
    std::size_t cursor = begin;
    bool samplingFrequencySeen = false;
    bool outputSamplingFrequencySeen = false;
    bool channelsSeen = false;
    bool crcSeen = false;
    while (cursor < end) {
        MemoryElement element;
        if (!nextMemoryElement(data, cursor, end, element)) {
            return false;
        }
        const auto* payload = data.data() + element.payload;
        const std::size_t size = element.end - element.payload;
        if (element.id == ID_SAMPLING_FREQUENCY) {
            if (samplingFrequencySeen || (size != 4 && size != 8)) {
                return false;
            }
            samplingFrequencySeen = true;
            track.samplingFrequency = readFloat(payload, size);
        } else if (element.id == ID_OUTPUT_SAMPLING_FREQUENCY) {
            if (outputSamplingFrequencySeen || (size != 4 && size != 8)) {
                return false;
            }
            outputSamplingFrequencySeen = true;
            track.outputSamplingFrequency = readFloat(payload, size);
        } else if (element.id == ID_CHANNELS) {
            if (channelsSeen || size == 0 || size > 8) {
                return false;
            }
            channelsSeen = true;
            track.channels = readUnsigned(payload, size);
        } else if (element.id == ID_CRC32) {
            if (crcSeen || !validateLeadingMemoryCrc(data, element, begin, end)) {
                return false;
            }
            crcSeen = true;
        }
    }
    return samplingFrequencySeen && channelsSeen;
}

bool parseTrackEntry(
    const std::vector<std::uint8_t>& data,
    std::size_t begin,
    std::size_t end,
    SequentialTrackInfo& track) {
    std::size_t cursor = begin;
    bool trackNumberSeen = false;
    bool trackUidSeen = false;
    bool trackTypeSeen = false;
    bool codecIdSeen = false;
    bool codecPrivateSeen = false;
    bool defaultDurationSeen = false;
    bool codecDelaySeen = false;
    bool seekPreRollSeen = false;
    bool timestampScaleSeen = false;
    bool audioSeen = false;
    bool crcSeen = false;
    while (cursor < end) {
        MemoryElement element;
        if (!nextMemoryElement(data, cursor, end, element)) {
            return false;
        }
        const auto* payload = data.data() + element.payload;
        const std::size_t size = element.end - element.payload;
        if (element.id == ID_TRACK_NUMBER) {
            if (trackNumberSeen || size == 0 || size > 8) {
                return false;
            }
            trackNumberSeen = true;
            track.trackNumber = readUnsigned(payload, size);
        } else if (element.id == ID_TRACK_UID) {
            if (trackUidSeen || size == 0 || size > 8) {
                return false;
            }
            trackUidSeen = true;
            track.trackUid = readUnsigned(payload, size);
        } else if (element.id == ID_TRACK_TYPE) {
            if (trackTypeSeen || size == 0 || size > 8) {
                return false;
            }
            trackTypeSeen = true;
            track.trackType = readUnsigned(payload, size);
        } else if (element.id == ID_CODEC_ID) {
            if (codecIdSeen || size == 0) {
                return false;
            }
            codecIdSeen = true;
            track.codecId.assign(reinterpret_cast<const char*>(payload), size);
        } else if (element.id == ID_CODEC_PRIVATE) {
            if (codecPrivateSeen || size == 0) {
                return false;
            }
            codecPrivateSeen = true;
            track.codecPrivate.assign(payload, payload + size);
        } else if (element.id == ID_DEFAULT_DURATION) {
            if (defaultDurationSeen || size == 0 || size > 8) {
                return false;
            }
            defaultDurationSeen = true;
            track.defaultDurationNs = readUnsigned(payload, size);
        } else if (element.id == ID_CODEC_DELAY) {
            if (codecDelaySeen || size == 0 || size > 8) {
                return false;
            }
            codecDelaySeen = true;
            track.codecDelayNs = readUnsigned(payload, size);
        } else if (element.id == ID_SEEK_PREROLL) {
            if (seekPreRollSeen || size == 0 || size > 8) {
                return false;
            }
            seekPreRollSeen = true;
            track.seekPreRollNs = readUnsigned(payload, size);
        } else if (element.id == ID_TRACK_TIMESTAMP_SCALE) {
            if (timestampScaleSeen || (size != 4 && size != 8)) {
                return false;
            }
            timestampScaleSeen = true;
            track.trackTimestampScale = readFloat(payload, size);
        } else if (element.id == ID_AUDIO) {
            if (audioSeen || !parseAudio(data, element.payload, element.end, track)) {
                return false;
            }
            audioSeen = true;
        } else if (element.id == ID_CRC32) {
            if (crcSeen || !validateLeadingMemoryCrc(data, element, begin, end)) {
                return false;
            }
            crcSeen = true;
        }
    }
    return trackNumberSeen && trackUidSeen && trackTypeSeen && codecIdSeen &&
        track.trackNumber > 0 && track.trackUid > 0 && !track.codecId.empty();
}

bool parseTracksPayload(
    const std::vector<std::uint8_t>& data,
    std::vector<SequentialTrackInfo>& tracks) {
    std::size_t cursor = 0;
    bool crcSeen = false;
    while (cursor < data.size()) {
        MemoryElement element;
        if (!nextMemoryElement(data, cursor, data.size(), element)) {
            return false;
        }
        if (element.id == ID_TRACK_ENTRY) {
            SequentialTrackInfo track;
            if (!parseTrackEntry(data, element.payload, element.end, track)) {
                return false;
            }
            tracks.push_back(std::move(track));
        } else if (element.id == ID_CRC32) {
            if (crcSeen || !validateLeadingMemoryCrc(
                    data, element, 0, data.size())) {
                return false;
            }
            crcSeen = true;
        }
    }
    return !tracks.empty();
}

bool parseInfoPayload(
    const std::vector<std::uint8_t>& data,
    SequentialScanState& result) {
    std::size_t cursor = 0;
    bool timestampScaleSeen = false;
    bool durationSeen = false;
    bool crcSeen = false;
    while (cursor < data.size()) {
        MemoryElement element;
        if (!nextMemoryElement(data, cursor, data.size(), element)) {
            return false;
        }
        const auto* payload = data.data() + element.payload;
        const std::size_t size = element.end - element.payload;
        if (element.id == ID_TIMESTAMP_SCALE) {
            if (timestampScaleSeen || size == 0 || size > 8) {
                return false;
            }
            timestampScaleSeen = true;
            result.timestampScaleNs = readUnsigned(payload, size);
        } else if (element.id == ID_DURATION) {
            if (durationSeen || (size != 4 && size != 8)) {
                return false;
            }
            durationSeen = true;
            result.segmentDurationTicks = readFloat(payload, size);
        } else if (element.id == ID_CRC32) {
            if (crcSeen || !validateLeadingMemoryCrc(
                    data, element, 0, data.size())) {
                return false;
            }
            crcSeen = true;
        }
    }
    return result.timestampScaleNs > 0 && std::isfinite(result.segmentDurationTicks);
}

bool isMatroskaDocType(const std::vector<std::uint8_t>& data) {
    std::size_t cursor = 0;
    bool found = false;
    bool crcSeen = false;
    while (cursor < data.size()) {
        MemoryElement element;
        if (!nextMemoryElement(data, cursor, data.size(), element)) {
            return false;
        }
        if (element.id == ID_DOC_TYPE) {
            if (found) {
                return false;
            }
            found = true;
            const std::size_t size = element.end - element.payload;
            const std::string docType(
                reinterpret_cast<const char*>(data.data() + element.payload),
                size);
            if (docType != "matroska") {
                return false;
            }
        } else if (element.id == ID_CRC32) {
            if (crcSeen || !validateLeadingMemoryCrc(
                    data, element, 0, data.size())) {
                return false;
            }
            crcSeen = true;
        }
    }
    return found;
}

bool readSmallPayload(
    ForwardFileReader& reader,
    std::uint64_t size,
    std::vector<std::uint8_t>& data,
    std::uint64_t& maximumWorking) {
    if (size > MAX_SMALL_MASTER || size > (std::numeric_limits<std::size_t>::max)()) {
        return false;
    }
    data.resize(static_cast<std::size_t>(size));
    maximumWorking = (std::max<std::uint64_t>)(maximumWorking, data.size());
    return reader.readExact(data.data(), data.size());
}

struct ParsedBlock {
    bool valid = false;
    std::uint64_t trackNumber = 0;
    std::int16_t relativeTimestamp = 0;
    std::uint8_t flags = 0;
    std::uint64_t laceCount = 0;
};

ParsedBlock readBlockLayout(ForwardFileReader& reader, std::uint64_t size) {
    ParsedBlock result;
    const std::uint64_t start = reader.position();
    std::uint64_t end = 0;
    if (size == 0 || size > MAX_BLOCK_BYTES || !checkedAdd(start, size, end)) {
        return result;
    }
    const EbmlVint track = readEbmlVint(reader, false);
    if (!track.valid || track.unknown || track.value == 0 ||
        reader.position() > end || end - reader.position() < 3) {
        return result;
    }
    result.trackNumber = track.value;
    std::uint8_t header[3]{};
    if (!reader.readExact(header, sizeof(header))) {
        return result;
    }
    result.relativeTimestamp = static_cast<std::int16_t>(
        static_cast<std::uint16_t>(header[0]) << 8 |
        static_cast<std::uint16_t>(header[1]));
    result.flags = header[2];
    const int lacing = (result.flags >> 1) & 0x03;
    if (lacing == 0) {
        if (reader.position() >= end || !reader.skip(end - reader.position())) {
            return result;
        }
        result.laceCount = 1;
        result.valid = true;
        return result;
    }
    std::uint8_t laceCountMinusOne = 0;
    if (reader.position() >= end || !reader.readByte(laceCountMinusOne)) {
        return result;
    }
    const std::uint64_t laceCount =
        static_cast<std::uint64_t>(laceCountMinusOne) + 1U;
    if (laceCount < 2) {
        return result;
    }
    std::uint64_t declared = 0;
    if (lacing == 1) {
        for (std::uint64_t lace = 0; lace + 1 < laceCount; ++lace) {
            std::uint64_t laceSize = 0;
            std::uint8_t byte = 255;
            while (byte == 255) {
                if (reader.position() >= end ||
                    reader.position() - start > MAX_LACING_HEADER_BYTES ||
                    !reader.readByte(byte)) {
                    return result;
                }
                if (!checkedAdd(laceSize, byte, laceSize)) {
                    return result;
                }
            }
            if (laceSize == 0 || !checkedAdd(declared, laceSize, declared)) {
                return result;
            }
        }
    } else if (lacing == 2) {
        const std::uint64_t payload = end - reader.position();
        if (payload == 0 || payload % laceCount != 0) {
            return result;
        }
        if (!reader.skip(payload)) {
            return result;
        }
        result.laceCount = laceCount;
        result.valid = true;
        return result;
    } else {
        const EbmlVint first = readEbmlVint(reader, false);
        if (!first.valid || first.unknown || first.value == 0 ||
            first.value >
                static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
            return result;
        }
        declared = first.value;
        std::int64_t prior = static_cast<std::int64_t>(first.value);
        for (std::uint64_t lace = 1; lace + 1 < laceCount; ++lace) {
            if (reader.position() - start > MAX_LACING_HEADER_BYTES) {
                return result;
            }
            const EbmlVint encoded = readEbmlVint(reader, false);
            if (!encoded.valid || encoded.unknown) {
                return result;
            }
            const std::uint64_t bias =
                (std::uint64_t{1} << (7U * encoded.length - 1U)) - 1U;
            if (encoded.value >
                static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) +
                    bias) {
                return result;
            }
            const std::int64_t delta = static_cast<std::int64_t>(encoded.value) -
                static_cast<std::int64_t>(bias);
            if ((delta < 0 && prior < -delta) ||
                (delta > 0 && prior > (std::numeric_limits<std::int64_t>::max)() - delta)) {
                return result;
            }
            const std::int64_t next = prior + delta;
            if (next <= 0 || !checkedAdd(declared, static_cast<std::uint64_t>(next), declared)) {
                return result;
            }
            prior = next;
        }
    }
    if (reader.position() > end || declared >= end - reader.position()) {
        return result;
    }
    const std::uint64_t last = end - reader.position() - declared;
    if (last == 0) {
        return result;
    }
    if (!reader.skip(end - reader.position())) {
        return result;
    }
    result.laceCount = laceCount;
    result.valid = true;
    return result;
}

bool delayToFrames(std::uint64_t ns, int sampleRate, std::uint64_t& frames) noexcept {
    if (sampleRate <= 0 || ns >
        (std::numeric_limits<std::uint64_t>::max)() /
            static_cast<std::uint64_t>(sampleRate)) {
        return false;
    }
    const std::uint64_t scaled = ns * static_cast<std::uint64_t>(sampleRate);
    if (scaled > (std::numeric_limits<std::uint64_t>::max)() - 500000000U) {
        return false;
    }
    frames = (scaled + 500000000U) / 1000000000U;
    if (frames > (std::numeric_limits<std::uint64_t>::max)() / 1000000000U) {
        return false;
    }
    const std::uint64_t reconstructed = frames * 1000000000U;
    const std::uint64_t error = reconstructed > scaled
        ? reconstructed - scaled
        : scaled - reconstructed;
    return error < 500000000U;
}

struct ScanState {
    SequentialScanState* result = nullptr;
    const SequentialSelectedStream* selected = nullptr;
    bool selectedTrackSeen = false;
    bool selectedBlockSeen = false;
    std::optional<std::int64_t> previousTimestampNs;
    std::uint64_t previousAccessUnits = 0;
    std::int64_t currentLastDiscardPaddingNs = 0;
    bool currentLastDiscardPaddingPresent = false;
    bool fatal = false;
};

void fail(ScanState& state, std::string status, std::string reason) {
    if (!state.fatal) {
        state.result->status = std::move(status);
        state.result->reason = std::move(reason);
        state.fatal = true;
    }
}

void processSelectedBlock(
    ScanState& state,
    const ParsedBlock& block,
    std::uint64_t clusterTimestamp,
    std::optional<std::uint64_t> blockDurationTicks,
    std::int64_t discardPaddingNs,
    bool discardPaddingPresent) {
    auto& result = *state.result;
    if (!block.valid) {
        result.allLacingValid = false;
        fail(state, "invalid_media", "invalid_block_or_lacing");
        return;
    }
    ++result.blocksParsed;
    const bool declaredTrack = std::any_of(
        result.tracks.begin(),
        result.tracks.end(),
        [&](const SequentialTrackInfo& track) {
            return track.trackNumber == block.trackNumber;
        });
    if (!declaredTrack) {
        fail(state, "invalid_media", "block_references_undeclared_track");
        return;
    }
    if (block.trackNumber != result.selectedTrackNumber) {
        return;
    }
    if (state.currentLastDiscardPaddingPresent) {
        fail(state, "conflict", "discard_padding_before_final_selected_block");
        return;
    }
    ++result.selectedBlocks;
    if (block.laceCount > 1) {
        ++result.lacedSelectedBlocks;
    }
    const std::uint64_t accessUnits = block.laceCount;
    if (accessUnits == 0 || result.selectedAccessUnits >
        (std::numeric_limits<std::uint64_t>::max)() - accessUnits) {
        fail(state, "conflict", "access_unit_count_overflow");
        return;
    }
    const std::int64_t ticks = static_cast<std::int64_t>(clusterTimestamp) +
        block.relativeTimestamp;
    if (ticks < 0 || result.timestampScaleNs >
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) /
            static_cast<std::uint64_t>(ticks == 0 ? 1 : ticks)) {
        fail(state, "conflict", "timestamp_overflow");
        return;
    }
    const std::int64_t timestampNs = ticks *
        static_cast<std::int64_t>(result.timestampScaleNs);
    if (state.previousTimestampNs) {
        const std::int64_t delta = timestampNs - *state.previousTimestampNs;
        const long double expected =
            static_cast<long double>(state.previousAccessUnits) *
            result.aac.samplesPerAccessUnit * 1000000000.0L /
            result.aac.sampleRate;
        const long double tolerance = static_cast<long double>(result.timestampScaleNs) + 1.0L;
        if (delta <= 0 || std::fabs(static_cast<long double>(delta) - expected) > tolerance) {
            result.selectedTimestampContinuity = false;
            fail(state, "conflict", delta <= 0 ? "selected_timestamp_overlap" : "selected_timestamp_gap");
            return;
        }
    } else {
        result.firstSelectedTimestampNs = timestampNs;
    }
    if (blockDurationTicks) {
        const long double declared =
            static_cast<long double>(*blockDurationTicks) * result.timestampScaleNs;
        const long double expected =
            static_cast<long double>(accessUnits) *
            result.aac.samplesPerAccessUnit * 1000000000.0L /
            result.aac.sampleRate;
        if (std::fabs(declared - expected) > result.timestampScaleNs + 1.0L) {
            fail(state, "conflict", "block_duration_conflict");
            return;
        }
    }
    state.previousTimestampNs = timestampNs;
    state.previousAccessUnits = accessUnits;
    result.lastSelectedTimestampNs = timestampNs;
    result.selectedAccessUnits += accessUnits;
    state.currentLastDiscardPaddingNs = discardPaddingNs;
    state.currentLastDiscardPaddingPresent = discardPaddingPresent;
    if (discardPaddingPresent) {
        result.discardPaddingPresent = true;
    }
    state.selectedBlockSeen = true;
}

bool parseBlockGroup(
    ForwardFileReader& reader,
    std::uint64_t groupPayloadOffset,
    std::uint64_t groupEnd,
    std::uint64_t clusterTimestamp,
    ScanState& state) {
    std::optional<ParsedBlock> block;
    std::optional<std::uint64_t> duration;
    std::optional<std::int64_t> discardPadding;
    std::optional<std::uint32_t> crc;
    while (reader.position() < groupEnd && !state.fatal) {
        EbmlElementHeader child;
        if (!readEbmlElementHeader(reader, child)) {
            return false;
        }
        ++state.result->elementsParsed;
        std::uint64_t childEnd = 0;
        if (!payloadEnd(child, groupEnd, childEnd)) {
            return false;
        }
        if (child.id == ID_BLOCK) {
            if (block) {
                fail(state, "invalid_media", "duplicate_block_in_group");
                return false;
            }
            block = readBlockLayout(reader, child.size);
        } else if (child.id == ID_BLOCK_DURATION || child.id == ID_DISCARD_PADDING) {
            if (child.size == 0 || child.size > 8) {
                return false;
            }
            std::uint8_t data[8]{};
            if (!reader.readExact(data, static_cast<std::size_t>(child.size))) {
                return false;
            }
            if (child.id == ID_BLOCK_DURATION) {
                if (duration) {
                    fail(state, "invalid_media", "duplicate_block_duration");
                    return false;
                }
                duration = readUnsigned(data, static_cast<std::size_t>(child.size));
            } else {
                if (discardPadding) {
                    fail(state, "invalid_media", "duplicate_discard_padding");
                    return false;
                }
                discardPadding =
                    readSigned(data, static_cast<std::size_t>(child.size));
            }
        } else if (child.id == ID_CRC32) {
            if (child.offset != groupPayloadOffset || child.size != 4 || crc) {
                return false;
            }
            std::uint8_t data[4]{};
            if (!reader.readExact(data, sizeof(data))) {
                return false;
            }
            crc = readCrc32Value(data);
            reader.beginCrc32();
        } else if (!reader.skip(child.size)) {
            return false;
        }
    }
    if (reader.position() != groupEnd || !block ||
        (crc && !reader.endCrc32(*crc))) {
        return false;
    }
    processSelectedBlock(
        state,
        *block,
        clusterTimestamp,
        duration,
        discardPadding.value_or(0),
        discardPadding.has_value());
    return true;
}

bool parseCluster(
    ForwardFileReader& reader,
    const EbmlElementHeader& cluster,
    std::uint64_t clusterEnd,
    ScanState& state) {
    if (cluster.unknownSize) {
        state.result->clusterSizeUnknown = true;
        fail(state, "unsupported", "unknown_size_cluster_not_supported");
        return false;
    }
    std::uint64_t timestamp = 0;
    bool timestampSeen = false;
    std::optional<std::uint32_t> crc;
    while (reader.position() < clusterEnd && !state.fatal) {
        EbmlElementHeader child;
        if (!readEbmlElementHeader(reader, child)) {
            return false;
        }
        ++state.result->elementsParsed;
        std::uint64_t childEnd = 0;
        if (!payloadEnd(child, clusterEnd, childEnd)) {
            return false;
        }
        if (child.id == ID_CLUSTER_TIMESTAMP) {
            if (timestampSeen || child.size == 0 || child.size > 8) {
                return false;
            }
            std::uint8_t data[8]{};
            if (!reader.readExact(data, static_cast<std::size_t>(child.size))) {
                return false;
            }
            timestamp = readUnsigned(data, static_cast<std::size_t>(child.size));
            timestampSeen = true;
        } else if (child.id == ID_SIMPLE_BLOCK) {
            if (!timestampSeen) {
                fail(state, "invalid_media", "block_before_cluster_timestamp");
                return false;
            }
            processSelectedBlock(
                state,
                readBlockLayout(reader, child.size),
                timestamp,
                std::nullopt,
                0,
                false);
        } else if (child.id == ID_BLOCK_GROUP) {
            if (!timestampSeen ||
                !parseBlockGroup(
                    reader,
                    child.payloadOffset,
                    childEnd,
                    timestamp,
                    state)) {
                return false;
            }
        } else if (child.id == ID_CRC32) {
            state.result->crcElementPresent = true;
            if (child.offset != cluster.payloadOffset || child.size != 4 || crc) {
                fail(state, "invalid_media", "invalid_or_duplicate_cluster_crc");
                return false;
            }
            std::uint8_t data[4]{};
            if (!reader.readExact(data, sizeof(data))) {
                return false;
            }
            crc = readCrc32Value(data);
            reader.beginCrc32();
        } else if (!reader.skip(child.size)) {
            return false;
        }
    }
    if (reader.position() != clusterEnd) {
        return false;
    }
    if (crc && !reader.endCrc32(*crc)) {
        fail(state, "invalid_media", "cluster_crc_mismatch");
        return false;
    }
    return true;
}

}  // namespace

SequentialScanState scanMatroskaSelectedAacImpl(
    const std::filesystem::path& path,
    const SequentialSelectedStream& selected,
    std::size_t bufferBytes,
    std::uint64_t forceReadErrorAfterBytes) {
    const auto wallBegin = Clock::now();
    const double cpuBegin = threadCpuMs();
    SequentialScanState result;
    ScanState state{&result, &selected};
    ForwardFileReader reader(path, bufferBytes, forceReadErrorAfterBytes);
    result.maximumWorkingBytes = reader.bufferBytes();
    if (!reader.valid()) {
        result.status = "io_error";
        result.reason = reader.error();
        return result;
    }
    result.fileSizeBytes = reader.fileSize();
    std::vector<std::uint8_t> metadataPayload(
        static_cast<std::size_t>(MAX_SMALL_MASTER));
    metadataPayload.clear();
    if (!checkedAdd(
            result.maximumWorkingBytes,
            metadataPayload.capacity(),
            result.maximumWorkingBytes)) {
        fail(state, "conflict", "working_buffer_size_overflow");
    }

    EbmlElementHeader ebml;
    if (!readEbmlElementHeader(reader, ebml) || ebml.id != ID_EBML || ebml.unknownSize) {
        fail(state, "invalid_media", "ebml_header_missing");
    } else {
        ++result.elementsParsed;
        std::uint64_t ebmlEnd = 0;
        if (!payloadEnd(ebml, result.fileSizeBytes, ebmlEnd) ||
            !readSmallPayload(
                reader,
                ebml.size,
                metadataPayload,
                result.maximumWorkingBytes)) {
            fail(state, "invalid_media", "invalid_ebml_header_size");
        } else if (!isMatroskaDocType(metadataPayload)) {
            fail(state, "unsupported_early", "not_matroska");
        }
    }

    EbmlElementHeader segment;
    if (!state.fatal &&
        (!readEbmlElementHeader(reader, segment) || segment.id != ID_SEGMENT)) {
        fail(state, "invalid_media", "segment_missing");
    }
    std::uint64_t segmentEnd = result.fileSizeBytes;
    if (!state.fatal) {
        ++result.elementsParsed;
        result.segmentSizeUnknown = segment.unknownSize;
        if (!segment.unknownSize &&
            (!checkedAdd(segment.payloadOffset, segment.size, segmentEnd) ||
             segmentEnd != result.fileSizeBytes)) {
            fail(state, "invalid_media", "segment_does_not_cover_file");
        }
    }

    bool infoParsed = false;
    bool tracksParsed = false;
    while (!state.fatal && reader.position() < segmentEnd) {
        EbmlElementHeader element;
        if (!readEbmlElementHeader(reader, element)) {
            fail(state, "invalid_media", "invalid_segment_element");
            break;
        }
        ++result.elementsParsed;
        if (element.id == ID_CLUSTER && element.unknownSize) {
            result.clusterSizeUnknown = true;
            fail(state, "unsupported", "unknown_size_cluster");
            break;
        }
        std::uint64_t end = 0;
        if (!payloadEnd(element, segmentEnd, end)) {
            fail(state, "invalid_media", "element_exceeds_segment");
            break;
        }
        if (element.id == ID_INFO || element.id == ID_TRACKS) {
            if (!readSmallPayload(
                    reader,
                    element.size,
                    metadataPayload,
                    result.maximumWorkingBytes)) {
                fail(state, "unsupported", "metadata_element_too_large");
                break;
            }
            if (element.id == ID_INFO) {
                if (infoParsed || !parseInfoPayload(metadataPayload, result)) {
                    fail(state, "invalid_media", "invalid_info");
                } else {
                    infoParsed = true;
                }
            } else {
                if (tracksParsed ||
                    !parseTracksPayload(metadataPayload, result.tracks)) {
                    fail(state, "invalid_media", "invalid_tracks");
                    break;
                }
                tracksParsed = true;
                std::size_t matches = 0;
                int audioOrdinal = 0;
                for (const auto& track : result.tracks) {
                    if (track.trackType != 2) {
                        continue;
                    }
                    const bool selectedOrdinal = audioOrdinal++ == selected.audioOrdinal;
                    const bool selectedNumber = selected.trackNumber == 0 ||
                        track.trackNumber == selected.trackNumber;
                    if (selectedOrdinal && selectedNumber && track.codecId == "A_AAC" &&
                        track.codecPrivate == selected.codecPrivate &&
                        static_cast<int>(std::llround(track.samplingFrequency)) == selected.sampleRate &&
                        static_cast<int>(track.channels) == selected.channels &&
                        track.trackTimestampScale == 1.0 &&
                        (track.outputSamplingFrequency == 0.0 ||
                         static_cast<int>(std::llround(track.outputSamplingFrequency)) ==
                             selected.sampleRate)) {
                        ++matches;
                        result.selectedTrackNumber = track.trackNumber;
                        result.selectedTrackUid = track.trackUid;
                        result.codecDelayNs = track.codecDelayNs;
                        result.aac = parseAacAudioSpecificConfig(
                            track.codecPrivate.data(), track.codecPrivate.size());
                    }
                }
                result.mappingProven = matches == 1;
                if (!result.mappingProven) {
                    fail(state, "conflict", "selected_track_mapping_conflict");
                } else if (!result.aac.valid || !result.aac.supported) {
                    fail(state, "unsupported", result.aac.reason);
                } else if (result.aac.audioObjectType != 2) {
                    fail(state, "unsupported", "aac_profile_unsupported");
                } else if (result.aac.frameLengthFlag != 0 ||
                           result.aac.samplesPerAccessUnit != 1024) {
                    fail(state, "unsupported", "aac_frame_length_unsupported");
                } else if (result.aac.sampleRate != selected.sampleRate) {
                    fail(state, "conflict", "aac_sample_rate_conflict");
                } else if (result.aac.channelConfiguration != selected.channels) {
                    fail(state, "conflict", "aac_channel_conflict");
                } else {
                    std::uint64_t delayFrames = 0;
                    if (!delayToFrames(result.codecDelayNs, selected.sampleRate, delayFrames) ||
                        delayFrames != static_cast<std::uint64_t>((std::max)(0, selected.initialPadding))) {
                        fail(state, "conflict", "codec_delay_initial_padding_conflict");
                    } else {
                        result.initialSkipFrames = delayFrames;
                    }
                }
            }
        } else if (element.id == ID_CLUSTER) {
            if (!tracksParsed) {
                fail(state, "unsupported", "cluster_before_tracks");
                break;
            }
            ++result.clustersParsed;
            if (!parseCluster(reader, element, end, state)) {
                if (!state.fatal) {
                    fail(state, "invalid_media", "invalid_cluster");
                }
                break;
            }
        } else if (element.id == ID_SEEK_HEAD) {
            result.seekHeadPresent = true;
            if (!reader.skip(element.size)) {
                fail(state, "invalid_media", "truncated_seek_head");
            }
        } else if (element.id == ID_CUES) {
            result.cuesPresent = true;
            if (!reader.skip(element.size)) {
                fail(state, "invalid_media", "truncated_cues");
            }
        } else if (element.id == ID_CRC32) {
            result.crcElementPresent = true;
            fail(state, "unsupported", "ebml_crc_validation_not_implemented");
        } else if (!reader.skip(element.size)) {
            fail(state, "invalid_media", "truncated_segment_payload");
        }
    }

    if (reader.error() == "forced_read_error") {
        result.status = "io_error";
        result.reason = "forced_read_error";
        state.fatal = true;
    } else if (reader.error() == "read_failed") {
        result.status = "io_error";
        result.reason = "input_read_failed";
        state.fatal = true;
    }
    result.reachedSegmentEnd = reader.position() == segmentEnd;
    result.bytesReturned = reader.bytesReturned();
    result.uniqueBytes = result.bytesReturned;
    result.duplicateBytes = 0;
    result.readCalls = reader.readCalls();
    if (!state.fatal) {
        result.reachedPhysicalEof = reader.eof();
        result.bytesReturned = reader.bytesReturned();
        result.uniqueBytes = result.bytesReturned;
        result.readCalls = reader.readCalls();
        if (!result.reachedPhysicalEof || result.bytesReturned != result.fileSizeBytes) {
            fail(state, "io_error", "physical_eof_not_reached");
        } else if (!reader.fileUnchanged()) {
            fail(state, "conflict", "input_changed_during_scan");
        }
    }
    if (!state.fatal && !state.selectedBlockSeen) {
        fail(state, "invalid_media", "selected_track_has_no_blocks");
    }
    if (!state.fatal && state.currentLastDiscardPaddingNs < 0) {
        fail(state, "unsupported", "negative_discard_padding_unsupported");
    }
    if (!state.fatal) {
        if (state.currentLastDiscardPaddingNs > 0) {
            std::uint64_t discardFrames = 0;
            if (!delayToFrames(
                    static_cast<std::uint64_t>(state.currentLastDiscardPaddingNs),
                    result.aac.sampleRate,
                    discardFrames)) {
                fail(state, "conflict", "discard_padding_not_sample_mappable");
            } else {
                result.terminalDiscardFrames = discardFrames;
            }
        } else {
            result.terminalZeroProvenByCompleteAbsence = !result.discardPaddingPresent;
        }
    }
    if (!state.fatal) {
        if (result.selectedAccessUnits >
            (std::numeric_limits<std::uint64_t>::max)() /
                static_cast<std::uint64_t>(result.aac.samplesPerAccessUnit)) {
            fail(state, "conflict", "physical_frame_count_overflow");
        } else {
            result.physicalFrames = result.selectedAccessUnits *
                static_cast<std::uint64_t>(result.aac.samplesPerAccessUnit);
            std::uint64_t trims = 0;
            if (!checkedAdd(
                    result.initialSkipFrames,
                    result.terminalDiscardFrames,
                    trims)) {
                fail(state, "conflict", "trim_arithmetic_overflow");
            }
            if (!state.fatal && trims >= result.physicalFrames) {
                fail(state, "invalid_media", "trim_exceeds_physical_frames");
            } else if (!state.fatal) {
                result.presentationFrames = result.physicalFrames - trims;
                result.status = "exact";
                result.reason = result.terminalDiscardFrames > 0
                    ? "complete_selected_track_with_terminal_discard"
                    : "complete_selected_track_with_declared_zero_terminal_trim";
            }
        }
    }
    result.lastDiscardPaddingNs = state.currentLastDiscardPaddingNs;
    result.scanWallMs = std::chrono::duration<double, std::milli>(
        Clock::now() - wallBegin).count();
    result.threadCpuMs = threadCpuMs() - cpuBegin;
    return result;
}

namespace {

bool formatListContains(const char* list, std::string_view expected) noexcept {
    if (!list || expected.empty()) {
        return false;
    }
    std::string_view remaining(list);
    while (!remaining.empty()) {
        const std::size_t comma = remaining.find(',');
        const std::string_view current = remaining.substr(0, comma);
        if (current == expected) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(comma + 1);
    }
    return false;
}

MatroskaAacSequentialReason mapScanReason(std::string_view reason) noexcept {
    if (reason == "complete_selected_track_with_terminal_discard") {
        return MatroskaAacSequentialReason::CompleteSelectedTrackWithTerminalDiscard;
    }
    if (reason == "complete_selected_track_with_declared_zero_terminal_trim") {
        return MatroskaAacSequentialReason::
            CompleteSelectedTrackWithDeclaredZeroTerminalTrim;
    }
    if (reason == "open_failed" || reason == "file_info_failed") {
        return MatroskaAacSequentialReason::InputOpenFailed;
    }
    if (reason == "input_read_failed") {
        return MatroskaAacSequentialReason::InputReadFailed;
    }
    if (reason == "forced_read_error") {
        return MatroskaAacSequentialReason::ForcedReadError;
    }
    if (reason == "physical_eof_not_reached") {
        return MatroskaAacSequentialReason::PhysicalEofNotReached;
    }
    if (reason == "input_changed_during_scan") {
        return MatroskaAacSequentialReason::InputChangedDuringScan;
    }
    if (reason == "not_matroska") {
        return MatroskaAacSequentialReason::UnsupportedDocType;
    }
    if (reason == "ebml_header_missing") {
        return MatroskaAacSequentialReason::EbmlHeaderMissing;
    }
    if (reason == "invalid_ebml_header_size") {
        return MatroskaAacSequentialReason::InvalidEbmlHeader;
    }
    if (reason == "segment_missing") {
        return MatroskaAacSequentialReason::SegmentMissing;
    }
    if (reason == "segment_does_not_cover_file") {
        return MatroskaAacSequentialReason::SegmentDoesNotCoverFile;
    }
    if (reason == "invalid_segment_element") {
        return MatroskaAacSequentialReason::InvalidSegmentElement;
    }
    if (reason == "element_exceeds_segment") {
        return MatroskaAacSequentialReason::ElementExceedsParent;
    }
    if (reason == "metadata_element_too_large") {
        return MatroskaAacSequentialReason::MetadataElementTooLarge;
    }
    if (reason == "invalid_info") {
        return MatroskaAacSequentialReason::InvalidInfo;
    }
    if (reason == "invalid_tracks") {
        return MatroskaAacSequentialReason::InvalidTracks;
    }
    if (reason == "cluster_before_tracks") {
        return MatroskaAacSequentialReason::ClusterBeforeTracks;
    }
    if (reason == "unknown_size_cluster" ||
        reason == "unknown_size_cluster_not_supported") {
        return MatroskaAacSequentialReason::UnknownSizeCluster;
    }
    if (reason == "invalid_cluster") {
        return MatroskaAacSequentialReason::InvalidCluster;
    }
    if (reason == "truncated_seek_head" ||
        reason == "truncated_cues" ||
        reason == "truncated_segment_payload") {
        return MatroskaAacSequentialReason::TruncatedElement;
    }
    if (reason == "invalid_or_duplicate_cluster_crc" ||
        reason == "ebml_crc_validation_not_implemented") {
        return MatroskaAacSequentialReason::InvalidOrDuplicateCrc;
    }
    if (reason == "cluster_crc_mismatch") {
        return MatroskaAacSequentialReason::CrcMismatch;
    }
    if (reason == "selected_track_mapping_conflict") {
        return MatroskaAacSequentialReason::SelectedTrackMappingConflict;
    }
    if (reason == "selected_track_has_no_blocks") {
        return MatroskaAacSequentialReason::SelectedTrackHasNoBlocks;
    }
    if (reason == "explicit_sbr_or_ps_unsupported" ||
        reason == "aac_profile_unsupported") {
        return MatroskaAacSequentialReason::AacProfileUnsupported;
    }
    if (reason == "aac_frame_length_unsupported") {
        return MatroskaAacSequentialReason::AacFrameLengthUnsupported;
    }
    if (reason == "aac_sample_rate_conflict") {
        return MatroskaAacSequentialReason::AacSampleRateConflict;
    }
    if (reason == "aac_channel_conflict") {
        return MatroskaAacSequentialReason::AacChannelConflict;
    }
    if (reason == "codec_delay_initial_padding_conflict") {
        return MatroskaAacSequentialReason::CodecDelayConflict;
    }
    if (reason == "block_references_undeclared_track") {
        return MatroskaAacSequentialReason::BlockReferencesUndeclaredTrack;
    }
    if (reason == "invalid_block_or_lacing") {
        return MatroskaAacSequentialReason::InvalidBlockOrLacing;
    }
    if (reason == "duplicate_block_in_group") {
        return MatroskaAacSequentialReason::DuplicateBlockInGroup;
    }
    if (reason == "duplicate_block_duration") {
        return MatroskaAacSequentialReason::DuplicateBlockDuration;
    }
    if (reason == "duplicate_discard_padding") {
        return MatroskaAacSequentialReason::DuplicateDiscardPadding;
    }
    if (reason == "block_before_cluster_timestamp") {
        return MatroskaAacSequentialReason::BlockBeforeClusterTimestamp;
    }
    if (reason == "block_duration_conflict") {
        return MatroskaAacSequentialReason::BlockDurationConflict;
    }
    if (reason == "selected_timestamp_gap") {
        return MatroskaAacSequentialReason::SelectedTimestampGap;
    }
    if (reason == "selected_timestamp_overlap") {
        return MatroskaAacSequentialReason::SelectedTimestampOverlap;
    }
    if (reason == "timestamp_overflow") {
        return MatroskaAacSequentialReason::SelectedTimestampOverflow;
    }
    if (reason == "access_unit_count_overflow") {
        return MatroskaAacSequentialReason::AccessUnitCountOverflow;
    }
    if (reason == "discard_padding_before_final_selected_block") {
        return MatroskaAacSequentialReason::DiscardPaddingBeforeFinalSelectedBlock;
    }
    if (reason == "negative_discard_padding_unsupported") {
        return MatroskaAacSequentialReason::NegativeDiscardPaddingUnsupported;
    }
    if (reason == "discard_padding_not_sample_mappable") {
        return MatroskaAacSequentialReason::DiscardPaddingNotSampleExact;
    }
    if (reason == "physical_frame_count_overflow") {
        return MatroskaAacSequentialReason::PhysicalFrameCountOverflow;
    }
    if (reason == "trim_arithmetic_overflow") {
        return MatroskaAacSequentialReason::TrimArithmeticOverflow;
    }
    if (reason == "trim_exceeds_physical_frames") {
        return MatroskaAacSequentialReason::TrimExceedsPhysicalFrames;
    }
    return MatroskaAacSequentialReason::InvalidSegmentElement;
}

MatroskaAacSequentialStatus mapScanStatus(std::string_view status) noexcept {
    if (status == "exact") {
        return MatroskaAacSequentialStatus::Exact;
    }
    if (status == "unsupported_early") {
        return MatroskaAacSequentialStatus::UnsupportedEarly;
    }
    if (status == "unsupported") {
        return MatroskaAacSequentialStatus::UnsupportedLate;
    }
    if (status == "conflict") {
        return MatroskaAacSequentialStatus::Conflict;
    }
    if (status == "invalid_media") {
        return MatroskaAacSequentialStatus::InvalidMedia;
    }
    return MatroskaAacSequentialStatus::IoError;
}

bool isArithmeticReason(MatroskaAacSequentialReason reason) noexcept {
    switch (reason) {
        case MatroskaAacSequentialReason::SelectedTimestampOverflow:
        case MatroskaAacSequentialReason::AccessUnitCountOverflow:
        case MatroskaAacSequentialReason::PhysicalFrameCountOverflow:
        case MatroskaAacSequentialReason::TrimArithmeticOverflow:
            return true;
        default:
            return false;
    }
}

}  // namespace

MatroskaAacSequentialEligibility evaluateMatroskaAacSequentialEligibility(
    const std::string& path,
    const AVFormatContext* formatContext,
    const AVStream* selectedAudioStream,
    bool strongerSampleExactAuthorityPresent) {
    MatroskaAacSequentialEligibility result;
    if (path.empty() || !formatContext || !formatContext->iformat ||
        !selectedAudioStream || !selectedAudioStream->codecpar ||
        strongerSampleExactAuthorityPresent ||
        !formatListContains(formatContext->iformat->name, "matroska")) {
        return result;
    }

    const AVCodecParameters* codecpar = selectedAudioStream->codecpar;
    if (codecpar->codec_type != AVMEDIA_TYPE_AUDIO ||
        codecpar->codec_id != AV_CODEC_ID_AAC ||
        codecpar->sample_rate <= 0 ||
        codecpar->ch_layout.nb_channels <= 0 ||
        codecpar->initial_padding < 0 ||
        !codecpar->extradata ||
        codecpar->extradata_size <= 0) {
        return result;
    }

    const auto config = parseAacAudioSpecificConfig(
        codecpar->extradata,
        static_cast<std::size_t>(codecpar->extradata_size));
    if (!config.valid) {
        result.reason = MatroskaAacSequentialReason::AacConfigInvalid;
        return result;
    }
    if (!config.supported || config.audioObjectType != 2) {
        result.reason = MatroskaAacSequentialReason::AacProfileUnsupported;
        return result;
    }
    if (config.frameLengthFlag != 0 || config.samplesPerAccessUnit != 1024) {
        result.reason = MatroskaAacSequentialReason::AacFrameLengthUnsupported;
        return result;
    }
    if (config.sampleRate != codecpar->sample_rate) {
        result.reason = MatroskaAacSequentialReason::AacSampleRateConflict;
        return result;
    }
    if (config.channelConfiguration != codecpar->ch_layout.nb_channels) {
        result.reason = MatroskaAacSequentialReason::AacChannelConflict;
        return result;
    }

    try {
        std::error_code error;
        const std::filesystem::path nativePath = std::filesystem::u8path(path);
        if (!std::filesystem::is_regular_file(nativePath, error) || error) {
            result.reason = MatroskaAacSequentialReason::InputNotRegularFile;
            return result;
        }
    } catch (...) {
        result.reason = MatroskaAacSequentialReason::InputNotRegularFile;
        return result;
    }

    int audioOrdinal = 0;
    bool selectedFound = false;
    for (unsigned index = 0; index < formatContext->nb_streams; ++index) {
        const AVStream* stream = formatContext->streams[index];
        const AVCodecParameters* current = stream ? stream->codecpar : nullptr;
        if (!current || current->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        if (stream == selectedAudioStream ||
            stream->index == selectedAudioStream->index) {
            selectedFound = true;
            break;
        }
        ++audioOrdinal;
    }
    if (!selectedFound) {
        result.reason =
            MatroskaAacSequentialReason::SelectedTrackMappingConflict;
        return result;
    }

    result.selected.streamIndex = static_cast<int>(selectedAudioStream->index);
    result.selected.audioOrdinal = audioOrdinal;
    result.selected.trackNumber = selectedAudioStream->id > 0
        ? static_cast<std::uint64_t>(selectedAudioStream->id)
        : 0;
    result.selected.sampleRate = codecpar->sample_rate;
    result.selected.channels = codecpar->ch_layout.nb_channels;
    result.selected.initialPadding = codecpar->initial_padding;
    result.selected.codecPrivate.assign(
        codecpar->extradata,
        codecpar->extradata + codecpar->extradata_size);
    result.eligible = true;
    result.reason =
        MatroskaAacSequentialReason::
            CompleteSelectedTrackWithDeclaredZeroTerminalTrim;
    return result;
}

MatroskaAacSequentialPresentationResult probeMatroskaAacSequentialPresentation(
    const std::string& path,
    const MatroskaAacSelectedStreamIdentity& selected,
    const MatroskaAacSequentialProbeOptions& options) {
    MatroskaAacSequentialPresentationResult result;
    if (selected.audioOrdinal < 0 || selected.sampleRate <= 0 ||
        selected.channels <= 0 || selected.codecPrivate.empty()) {
        return result;
    }

    SequentialSelectedStream scanSelected;
    scanSelected.streamIndex = selected.streamIndex;
    scanSelected.audioOrdinal = selected.audioOrdinal;
    scanSelected.trackNumber = selected.trackNumber;
    scanSelected.sampleRate = selected.sampleRate;
    scanSelected.channels = selected.channels;
    scanSelected.initialPadding = selected.initialPadding;
    scanSelected.codecPrivate = selected.codecPrivate;

    const std::uint64_t forcedReadError = options.testHooks
        ? options.testHooks->forceReadErrorAfterBytes
        : (std::numeric_limits<std::uint64_t>::max)();
    SequentialScanState scan;
    try {
        scan = scanMatroskaSelectedAacImpl(
            std::filesystem::u8path(path),
            scanSelected,
            (std::max)(options.readBufferBytes, std::size_t{1}),
            forcedReadError);
    } catch (const std::bad_alloc&) {
        result.status = MatroskaAacSequentialStatus::IoError;
        result.reason = MatroskaAacSequentialReason::MetadataElementTooLarge;
        return result;
    } catch (...) {
        result.status = MatroskaAacSequentialStatus::IoError;
        result.reason = MatroskaAacSequentialReason::InputReadFailed;
        return result;
    }

    result.status = mapScanStatus(scan.status);
    result.reason = mapScanReason(scan.reason);
    result.presentationFrames = scan.presentationFrames;
    result.physicalFrames = scan.physicalFrames;
    result.selectedAccessUnits = scan.selectedAccessUnits;
    result.samplesPerAccessUnit =
        static_cast<std::uint64_t>((std::max)(scan.aac.samplesPerAccessUnit, 0));
    result.initialSkipFrames = scan.initialSkipFrames;
    result.terminalDiscardFrames = scan.terminalDiscardFrames;
    result.trackNumber = scan.selectedTrackNumber;
    result.trackUid = scan.selectedTrackUid;
    result.codecDelayNanoseconds = scan.codecDelayNs;
    result.finalDiscardPaddingNanoseconds = scan.lastDiscardPaddingNs;
    result.sampleRate = scan.aac.sampleRate;
    result.channels = selected.channels;
    result.aacObjectType = scan.aac.audioObjectType;
    result.fileSizeBytes = scan.fileSizeBytes;
    result.bytesReturned = scan.bytesReturned;
    result.uniqueBytes = scan.uniqueBytes;
    result.duplicateBytes = scan.duplicateBytes;
    result.readCalls = scan.readCalls;
    result.seekCallsAfterOpen = scan.seekCallsAfterOpen;
    result.scanDurationUs = scan.scanWallMs > 0.0
        ? static_cast<std::uint64_t>(scan.scanWallMs * 1000.0)
        : 0;
    result.maximumWorkingBufferBytes = scan.maximumWorkingBytes;
    result.elementsParsed = scan.elementsParsed;
    result.clustersParsed = scan.clustersParsed;
    result.blocksParsed = scan.blocksParsed;
    result.selectedBlocks = scan.selectedBlocks;
    result.selectedLaces = scan.selectedAccessUnits;
    result.lacedSelectedBlocks = scan.lacedSelectedBlocks;
    result.reachedPhysicalEof = scan.reachedPhysicalEof;
    result.reachedSegmentEnd = scan.reachedSegmentEnd;
    result.segmentSizeUnknown = scan.segmentSizeUnknown;
    result.clusterSizeUnknown = scan.clusterSizeUnknown;
    result.selectedTrackMappingValid = scan.mappingProven;
    result.timestampContinuityValid = scan.selectedTimestampContinuity;
    result.allLacingValid = scan.allLacingValid;
    result.allRelevantCrcValid =
        result.reason != MatroskaAacSequentialReason::CrcMismatch &&
        result.reason != MatroskaAacSequentialReason::InvalidOrDuplicateCrc;
    result.crcFailures = result.allRelevantCrcValid ? 0 : 1;
    result.checkedArithmeticValid = !isArithmeticReason(result.reason);
    result.discardPaddingPresent = scan.discardPaddingPresent;
    result.terminalZeroProvenByCompleteAbsence =
        scan.terminalZeroProvenByCompleteAbsence;
    result.cuesPresent = scan.cuesPresent;
    result.seekHeadPresent = scan.seekHeadPresent;
    return result;
}

TotalPresentationEvidence makeMatroskaAacSequentialTotalPresentationEvidence(
    const MatroskaAacSequentialPresentationResult& result) noexcept {
    TotalPresentationEvidence evidence;
    if (!result.exact() || result.presentationFrames == 0 ||
        result.sampleRate <= 0) {
        return evidence;
    }
    evidence.frames = result.presentationFrames;
    evidence.trust = PresentationTotalTrust::SampleExact;
    evidence.source =
        PresentationTotalSource::MatroskaAacSequentialPresentation;
    evidence.domain = PresentationSampleDomain::NativeStreamSamples;
    evidence.sampleRate = result.sampleRate;
    evidence.exactRescale = true;
    evidence.validation = PresentationTotalValidation::SelfContainedMetadata;
    return evidence;
}

const char* matroskaAacSequentialStatusName(
    MatroskaAacSequentialStatus status) noexcept {
    switch (status) {
        case MatroskaAacSequentialStatus::Exact: return "exact";
        case MatroskaAacSequentialStatus::UnsupportedEarly:
            return "unsupported_early";
        case MatroskaAacSequentialStatus::UnsupportedLate:
            return "unsupported_late";
        case MatroskaAacSequentialStatus::Conflict: return "conflict";
        case MatroskaAacSequentialStatus::InvalidMedia: return "invalid_media";
        case MatroskaAacSequentialStatus::IoError: return "io_error";
    }
    return "unsupported_early";
}

const char* matroskaAacSequentialReasonName(
    MatroskaAacSequentialReason reason) noexcept {
    switch (reason) {
        case MatroskaAacSequentialReason::NotEligible: return "not_eligible";
        case MatroskaAacSequentialReason::CompleteSelectedTrackWithTerminalDiscard:
            return "complete_selected_track_with_terminal_discard";
        case MatroskaAacSequentialReason::
                CompleteSelectedTrackWithDeclaredZeroTerminalTrim:
            return "complete_selected_track_with_declared_zero_terminal_trim";
        case MatroskaAacSequentialReason::InputNotRegularFile:
            return "input_not_regular_file";
        case MatroskaAacSequentialReason::InputOpenFailed: return "input_open_failed";
        case MatroskaAacSequentialReason::InputReadFailed: return "input_read_failed";
        case MatroskaAacSequentialReason::ForcedReadError: return "forced_read_error";
        case MatroskaAacSequentialReason::InputChangedDuringScan:
            return "input_changed_during_scan";
        case MatroskaAacSequentialReason::PhysicalEofNotReached:
            return "physical_eof_not_reached";
        case MatroskaAacSequentialReason::NotMatroska: return "not_matroska";
        case MatroskaAacSequentialReason::UnsupportedDocType:
            return "unsupported_doc_type";
        case MatroskaAacSequentialReason::EbmlHeaderMissing:
            return "ebml_header_missing";
        case MatroskaAacSequentialReason::InvalidEbmlHeader:
            return "invalid_ebml_header";
        case MatroskaAacSequentialReason::SegmentMissing: return "segment_missing";
        case MatroskaAacSequentialReason::SegmentDoesNotCoverFile:
            return "segment_does_not_cover_file";
        case MatroskaAacSequentialReason::InvalidSegmentElement:
            return "invalid_segment_element";
        case MatroskaAacSequentialReason::ElementExceedsParent:
            return "element_exceeds_parent";
        case MatroskaAacSequentialReason::MetadataElementTooLarge:
            return "metadata_element_too_large";
        case MatroskaAacSequentialReason::InvalidInfo: return "invalid_info";
        case MatroskaAacSequentialReason::InvalidTracks: return "invalid_tracks";
        case MatroskaAacSequentialReason::TracksMissing: return "tracks_missing";
        case MatroskaAacSequentialReason::ClusterBeforeTracks:
            return "cluster_before_tracks";
        case MatroskaAacSequentialReason::UnknownSizeCluster:
            return "unknown_size_cluster";
        case MatroskaAacSequentialReason::InvalidCluster: return "invalid_cluster";
        case MatroskaAacSequentialReason::TruncatedElement:
            return "truncated_element";
        case MatroskaAacSequentialReason::InvalidOrDuplicateCrc:
            return "invalid_or_duplicate_crc";
        case MatroskaAacSequentialReason::CrcMismatch: return "crc_mismatch";
        case MatroskaAacSequentialReason::SelectedTrackMappingConflict:
            return "selected_track_mapping_conflict";
        case MatroskaAacSequentialReason::SelectedTrackAbsent:
            return "selected_track_absent";
        case MatroskaAacSequentialReason::SelectedTrackHasNoBlocks:
            return "selected_track_has_no_blocks";
        case MatroskaAacSequentialReason::CodecIdUnsupported:
            return "codec_id_unsupported";
        case MatroskaAacSequentialReason::CodecPrivateMismatch:
            return "codec_private_mismatch";
        case MatroskaAacSequentialReason::AacConfigInvalid:
            return "aac_config_invalid";
        case MatroskaAacSequentialReason::AacProfileUnsupported:
            return "aac_profile_unsupported";
        case MatroskaAacSequentialReason::AacFrameLengthUnsupported:
            return "aac_frame_length_unsupported";
        case MatroskaAacSequentialReason::AacSampleRateConflict:
            return "aac_sample_rate_conflict";
        case MatroskaAacSequentialReason::AacChannelConflict:
            return "aac_channel_conflict";
        case MatroskaAacSequentialReason::CodecDelayConflict:
            return "codec_delay_conflict";
        case MatroskaAacSequentialReason::CodecDelayNotSampleExact:
            return "codec_delay_not_sample_exact";
        case MatroskaAacSequentialReason::BlockReferencesUndeclaredTrack:
            return "block_references_undeclared_track";
        case MatroskaAacSequentialReason::InvalidBlockOrLacing:
            return "invalid_block_or_lacing";
        case MatroskaAacSequentialReason::BlockPayloadTooLarge:
            return "block_payload_too_large";
        case MatroskaAacSequentialReason::DuplicateBlockInGroup:
            return "duplicate_block_in_group";
        case MatroskaAacSequentialReason::DuplicateBlockDuration:
            return "duplicate_block_duration";
        case MatroskaAacSequentialReason::DuplicateDiscardPadding:
            return "duplicate_discard_padding";
        case MatroskaAacSequentialReason::BlockBeforeClusterTimestamp:
            return "block_before_cluster_timestamp";
        case MatroskaAacSequentialReason::BlockDurationConflict:
            return "block_duration_conflict";
        case MatroskaAacSequentialReason::SelectedTimestampGap:
            return "selected_timestamp_gap";
        case MatroskaAacSequentialReason::SelectedTimestampOverlap:
            return "selected_timestamp_overlap";
        case MatroskaAacSequentialReason::SelectedTimestampOverflow:
            return "selected_timestamp_overflow";
        case MatroskaAacSequentialReason::AccessUnitCountOverflow:
            return "access_unit_count_overflow";
        case MatroskaAacSequentialReason::DiscardPaddingBeforeFinalSelectedBlock:
            return "discard_padding_before_final_selected_block";
        case MatroskaAacSequentialReason::NegativeDiscardPaddingUnsupported:
            return "negative_discard_padding_unsupported";
        case MatroskaAacSequentialReason::DiscardPaddingNotSampleExact:
            return "discard_padding_not_sample_exact";
        case MatroskaAacSequentialReason::PhysicalFrameCountOverflow:
            return "physical_frame_count_overflow";
        case MatroskaAacSequentialReason::TrimArithmeticOverflow:
            return "trim_arithmetic_overflow";
        case MatroskaAacSequentialReason::TrimExceedsPhysicalFrames:
            return "trim_exceeds_physical_frames";
        case MatroskaAacSequentialReason::IndependentExactAuthorityConflict:
            return "independent_exact_authority_conflict";
    }
    return "not_eligible";
}

}  // namespace AveMediaBridge::Probe
