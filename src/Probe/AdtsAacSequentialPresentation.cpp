#include "AdtsAacSequentialPresentation.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace AveMediaBridge::Probe {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::array<int, 16> kAdtsSampleRates{
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000, 7350, 0, 0, 0,
};
constexpr std::uint64_t kMaximumProbeHandoffBytes = 1024 * 1024;

struct FileIdentity {
    std::uint64_t size = 0;
    std::uint64_t index = 0;
    std::uint64_t lastWriteTime = 0;
    std::uint32_t volumeSerial = 0;
};

FileIdentity identityFrom(const BY_HANDLE_FILE_INFORMATION& info) noexcept {
    FileIdentity result;
    result.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) |
        info.nFileSizeLow;
    result.index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) |
        info.nFileIndexLow;
    result.lastWriteTime =
        (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
        info.ftLastWriteTime.dwLowDateTime;
    result.volumeSerial = info.dwVolumeSerialNumber;
    return result;
}

bool sameIdentity(const FileIdentity& left, const FileIdentity& right) noexcept {
    return left.size == right.size && left.index == right.index &&
        left.lastWriteTime == right.lastWriteTime &&
        left.volumeSerial == right.volumeSerial;
}

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
        : handle_(handle) {}
    ~UniqueHandle() {
        if (valid()) {
            CloseHandle(handle_);
        }
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

enum class ReadStatus { Ok, Eof, Partial, Error };

class ForwardFileReader {
public:
    ForwardFileReader(
        const std::filesystem::path& path,
        std::size_t bufferBytes,
        std::uint64_t forceReadErrorAfterBytes)
        : handle_(CreateFileW(
              path.c_str(),
              GENERIC_READ,
              FILE_SHARE_READ,
              nullptr,
              OPEN_EXISTING,
              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
              nullptr)),
          buffer_((std::max)(bufferBytes, std::size_t{7})),
          forceReadErrorAfterBytes_(forceReadErrorAfterBytes) {
        if (!handle_.valid()) {
            reason_ = AdtsAacSequentialReason::InputOpenFailed;
            return;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (GetFileType(handle_.get()) != FILE_TYPE_DISK ||
            !GetFileInformationByHandle(handle_.get(), &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            reason_ = AdtsAacSequentialReason::InputNotDiskFile;
            return;
        }
        initialIdentity_ = identityFrom(info);
        valid_ = true;
    }

    bool valid() const noexcept { return valid_; }
    AdtsAacSequentialReason reason() const noexcept { return reason_; }
    const FileIdentity& initialIdentity() const noexcept { return initialIdentity_; }
    std::uint64_t bytesReturned() const noexcept { return bytesReturned_; }
    std::uint64_t readCalls() const noexcept { return readCalls_; }
    std::uint64_t maximumWorkingBufferBytes() const noexcept { return buffer_.size(); }

    ReadStatus readExact(std::uint8_t* destination, std::size_t bytes) {
        std::size_t copied = 0;
        while (copied < bytes) {
            if (cursor_ == available_) {
                const ReadStatus status = fill();
                if (status != ReadStatus::Ok) {
                    if (status == ReadStatus::Eof) {
                        return copied == 0 ? ReadStatus::Eof : ReadStatus::Partial;
                    }
                    return status;
                }
            }
            const std::size_t count =
                (std::min)(bytes - copied, available_ - cursor_);
            std::copy_n(buffer_.data() + cursor_, count, destination + copied);
            cursor_ += count;
            copied += count;
        }
        return ReadStatus::Ok;
    }

    ReadStatus skipExact(std::uint64_t bytes) {
        while (bytes > 0) {
            if (cursor_ == available_) {
                const ReadStatus status = fill();
                if (status != ReadStatus::Ok) {
                    return status == ReadStatus::Eof ? ReadStatus::Partial : status;
                }
            }
            const std::size_t count = static_cast<std::size_t>(
                (std::min<std::uint64_t>)(bytes, available_ - cursor_));
            cursor_ += count;
            bytes -= count;
        }
        return ReadStatus::Ok;
    }

    bool finalIdentity(FileIdentity& identity) const noexcept {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!valid_ || !GetFileInformationByHandle(handle_.get(), &info)) {
            return false;
        }
        identity = identityFrom(info);
        return true;
    }

private:
    ReadStatus fill() {
        if (bytesReturned_ >= forceReadErrorAfterBytes_) {
            return ReadStatus::Error;
        }
        const std::uint64_t beforeFault =
            forceReadErrorAfterBytes_ - bytesReturned_;
        const DWORD requested = static_cast<DWORD>(
            (std::min<std::uint64_t>)(buffer_.size(), beforeFault));
        if (requested == 0) {
            return ReadStatus::Error;
        }
        DWORD returned = 0;
        ++readCalls_;
        if (!ReadFile(handle_.get(), buffer_.data(), requested, &returned, nullptr)) {
            return ReadStatus::Error;
        }
        cursor_ = 0;
        available_ = returned;
        bytesReturned_ += returned;
        return returned == 0 ? ReadStatus::Eof : ReadStatus::Ok;
    }

    UniqueHandle handle_;
    std::vector<std::uint8_t> buffer_;
    std::size_t cursor_ = 0;
    std::size_t available_ = 0;
    FileIdentity initialIdentity_;
    std::uint64_t bytesReturned_ = 0;
    std::uint64_t readCalls_ = 0;
    std::uint64_t forceReadErrorAfterBytes_ =
        (std::numeric_limits<std::uint64_t>::max)();
    bool valid_ = false;
    AdtsAacSequentialReason reason_ =
        AdtsAacSequentialReason::FileIdentityUnavailable;
};

struct AdtsHeader {
    int mpegId = 0;
    int layer = 0;
    bool protectionAbsent = false;
    int audioObjectType = 0;
    int sampleRateIndex = 0;
    int sampleRate = 0;
    int channelConfiguration = 0;
    std::uint16_t frameLength = 0;
    int rawDataBlocks = 0;
};

bool supportedChannelCount(int channels) noexcept {
    return channels == 1 || channels == 2 || channels == 6;
}

int channelCountForConfiguration(int channelConfiguration) noexcept {
    switch (channelConfiguration) {
        case 1: return 1;
        case 2: return 2;
        case 6: return 6;
        default: return 0;
    }
}

AdtsAacSequentialReason parseAdtsHeader(
    const std::array<std::uint8_t, 7>& bytes,
    AdtsHeader& header) noexcept {
    header = {};
    if (bytes[0] != 0xff || (bytes[1] & 0xf0U) != 0xf0U) {
        return AdtsAacSequentialReason::InitialSyncMissing;
    }
    header.mpegId = (bytes[1] >> 3U) & 1U;
    header.layer = (bytes[1] >> 1U) & 3U;
    if (header.layer != 0) {
        return AdtsAacSequentialReason::InvalidLayer;
    }
    header.protectionAbsent = (bytes[1] & 1U) != 0;
    header.audioObjectType = ((bytes[2] >> 6U) & 3U) + 1U;
    header.sampleRateIndex = (bytes[2] >> 2U) & 15U;
    header.sampleRate = kAdtsSampleRates[header.sampleRateIndex];
    if (header.sampleRate <= 0) {
        return AdtsAacSequentialReason::InvalidSampleRateIndex;
    }
    header.channelConfiguration =
        ((bytes[2] & 1U) << 2U) | ((bytes[3] >> 6U) & 3U);
    header.frameLength = static_cast<std::uint16_t>(
        ((bytes[3] & 3U) << 11U) | (bytes[4] << 3U) | (bytes[5] >> 5U));
    header.rawDataBlocks = (bytes[6] & 3U) + 1U;
    const std::uint16_t headerBytes = header.protectionAbsent ? 7U : 9U;
    if (header.frameLength < headerBytes) {
        return AdtsAacSequentialReason::InvalidFrameLength;
    }
    return AdtsAacSequentialReason::CompleteStrictLcAdtsInventory;
}

bool sameConfiguration(const AdtsHeader& first, const AdtsHeader& current) noexcept {
    return first.mpegId == current.mpegId &&
        first.audioObjectType == current.audioObjectType &&
        first.sampleRateIndex == current.sampleRateIndex &&
        first.channelConfiguration == current.channelConfiguration &&
        first.protectionAbsent == current.protectionAbsent &&
        first.rawDataBlocks == current.rawDataBlocks;
}

AdtsAacSequentialReason changedConfigurationReason(
    const AdtsHeader& first,
    const AdtsHeader& current) noexcept {
    if (first.mpegId != current.mpegId) {
        return AdtsAacSequentialReason::MpegIdChanged;
    }
    if (first.audioObjectType != current.audioObjectType) {
        return AdtsAacSequentialReason::AacObjectTypeChanged;
    }
    if (first.sampleRateIndex != current.sampleRateIndex) {
        return AdtsAacSequentialReason::SampleRateChanged;
    }
    if (first.channelConfiguration != current.channelConfiguration) {
        return AdtsAacSequentialReason::ChannelConfigurationChanged;
    }
    if (first.protectionAbsent != current.protectionAbsent) {
        return AdtsAacSequentialReason::ProtectionModeChanged;
    }
    return AdtsAacSequentialReason::RawDataBlockModeChanged;
}

bool recognizedTrailingTag(const std::array<std::uint8_t, 7>& bytes) noexcept {
    return (bytes[0] == 'T' && bytes[1] == 'A' && bytes[2] == 'G') ||
        (bytes[0] == 'A' && bytes[1] == 'P' && bytes[2] == 'E' &&
         bytes[3] == 'T' && bytes[4] == 'A' && bytes[5] == 'G');
}

bool isIndependentExact(const TotalPresentationEvidence& evidence) noexcept {
    return evidence.trust == PresentationTotalTrust::SampleExact &&
        evidence.domain == PresentationSampleDomain::NativeStreamSamples &&
        evidence.sampleRate > 0 && evidence.exactRescale && !evidence.conflict;
}

bool readCurrentFileIdentity(const std::string& path, FileIdentity& identity) noexcept {
    try {
        const std::filesystem::path nativePath = std::filesystem::u8path(path);
        UniqueHandle handle(CreateFileW(
            nativePath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!handle.valid() || GetFileType(handle.get()) != FILE_TYPE_DISK ||
            !GetFileInformationByHandle(handle.get(), &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return false;
        }
        identity = identityFrom(info);
        return true;
    } catch (...) {
        return false;
    }
}

bool findUniqueJsonValue(
    const std::string& text,
    std::string_view key,
    std::size_t& valueOffset) {
    const std::string token = "\"" + std::string(key) + "\"";
    const std::size_t keyOffset = text.find(token);
    if (keyOffset == std::string::npos ||
        text.find(token, keyOffset + token.size()) != std::string::npos) {
        return false;
    }
    std::size_t cursor = keyOffset + token.size();
    while (cursor < text.size() &&
        (text[cursor] == ' ' || text[cursor] == '\t' ||
         text[cursor] == '\r' || text[cursor] == '\n')) {
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor++] != ':') {
        return false;
    }
    while (cursor < text.size() &&
        (text[cursor] == ' ' || text[cursor] == '\t' ||
         text[cursor] == '\r' || text[cursor] == '\n')) {
        ++cursor;
    }
    valueOffset = cursor;
    return cursor < text.size();
}

bool parseJsonUnsigned(
    const std::string& text,
    std::string_view key,
    std::uint64_t& value) {
    std::size_t cursor = 0;
    if (!findUniqueJsonValue(text, key, cursor) ||
        text[cursor] < '0' || text[cursor] > '9') {
        return false;
    }
    std::uint64_t parsed = 0;
    while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
        const std::uint64_t digit = static_cast<std::uint64_t>(text[cursor] - '0');
        if (parsed > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
        ++cursor;
    }
    value = parsed;
    return true;
}

bool parseJsonBool(const std::string& text, std::string_view key, bool& value) {
    std::size_t cursor = 0;
    if (!findUniqueJsonValue(text, key, cursor)) {
        return false;
    }
    if (text.compare(cursor, 4, "true") == 0) {
        value = true;
        return true;
    }
    if (text.compare(cursor, 5, "false") == 0) {
        value = false;
        return true;
    }
    return false;
}

bool parseJsonString(
    const std::string& text,
    std::string_view key,
    std::string& value) {
    std::size_t cursor = 0;
    if (!findUniqueJsonValue(text, key, cursor) || text[cursor++] != '"') {
        return false;
    }
    std::string parsed;
    while (cursor < text.size()) {
        const unsigned char ch = static_cast<unsigned char>(text[cursor++]);
        if (ch == '"') {
            value = std::move(parsed);
            return true;
        }
        if (ch < 0x20) {
            return false;
        }
        if (ch != '\\') {
            parsed.push_back(static_cast<char>(ch));
            continue;
        }
        if (cursor >= text.size()) {
            return false;
        }
        const char escaped = text[cursor++];
        switch (escaped) {
            case '"': parsed.push_back('"'); break;
            case '\\': parsed.push_back('\\'); break;
            case '/': parsed.push_back('/'); break;
            case 'b': parsed.push_back('\b'); break;
            case 'f': parsed.push_back('\f'); break;
            case 'n': parsed.push_back('\n'); break;
            case 'r': parsed.push_back('\r'); break;
            case 't': parsed.push_back('\t'); break;
            default: return false;
        }
    }
    return false;
}

bool readProbeDocument(const std::filesystem::path& path, std::string& text) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > kMaximumProbeHandoffBytes) {
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    text.resize(static_cast<std::size_t>(size));
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    return input && input.gcount() == static_cast<std::streamsize>(text.size());
}

AdtsAacSequentialPresentationResult finishTimed(
    AdtsAacSequentialPresentationResult result,
    Clock::time_point started) noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - started).count();
    result.scanDurationUs = elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
    return result;
}

}  // namespace

AdtsAacSequentialEligibility evaluateAdtsAacSequentialEligibility(
    const std::string& path,
    const AVFormatContext* formatContext,
    const AVStream* selectedAudioStream,
    bool strongerSampleExactAuthorityPresent) {
    AdtsAacSequentialEligibility result;
    if (strongerSampleExactAuthorityPresent) {
        result.reason = AdtsAacSequentialReason::StrongerAuthorityPresent;
        return result;
    }
    if (path.empty() || !formatContext || !formatContext->iformat ||
        !formatContext->iformat->name ||
        std::string_view(formatContext->iformat->name) != "aac") {
        result.reason = AdtsAacSequentialReason::NotStandaloneAdts;
        return result;
    }
    if (!selectedAudioStream || !selectedAudioStream->codecpar ||
        selectedAudioStream->index < 0) {
        result.reason = AdtsAacSequentialReason::SelectedStreamMissing;
        return result;
    }
    const AVCodecParameters* codecpar = selectedAudioStream->codecpar;
    if (codecpar->codec_type != AVMEDIA_TYPE_AUDIO ||
        codecpar->codec_id != AV_CODEC_ID_AAC) {
        result.reason = AdtsAacSequentialReason::CodecNotAac;
        return result;
    }
    if (codecpar->profile != AV_PROFILE_AAC_LOW) {
        result.reason = AdtsAacSequentialReason::AacProfileUnsupported;
        return result;
    }
    if (codecpar->frame_size != static_cast<int>(kAdtsAacSamplesPerRawDataBlock)) {
        result.reason = AdtsAacSequentialReason::AacFrameSizeUnsupported;
        return result;
    }
    if (codecpar->sample_rate <= 0) {
        result.reason = AdtsAacSequentialReason::OutputRateUnavailable;
        return result;
    }
    if (!supportedChannelCount(codecpar->ch_layout.nb_channels) ||
        !av_channel_layout_check(&codecpar->ch_layout)) {
        result.reason = AdtsAacSequentialReason::ChannelLayoutUnsupported;
        return result;
    }
    if (codecpar->initial_padding != 0 || codecpar->trailing_padding != 0) {
        result.reason = AdtsAacSequentialReason::CodecPaddingUnsupported;
        return result;
    }
    bool selectedFound = false;
    for (unsigned index = 0; index < formatContext->nb_streams; ++index) {
        const AVStream* stream = formatContext->streams[index];
        if (stream == selectedAudioStream ||
            (stream && stream->index == selectedAudioStream->index)) {
            selectedFound = true;
            break;
        }
    }
    if (!selectedFound) {
        result.reason = AdtsAacSequentialReason::SelectedStreamMissing;
        return result;
    }
    try {
        std::error_code error;
        const std::filesystem::path nativePath = std::filesystem::u8path(path);
        if (!std::filesystem::is_regular_file(nativePath, error) || error) {
            result.reason = AdtsAacSequentialReason::InputNotRegularFile;
            return result;
        }
    } catch (...) {
        result.reason = AdtsAacSequentialReason::InputNotRegularFile;
        return result;
    }

    result.selected.streamIndex = static_cast<int>(selectedAudioStream->index);
    result.selected.sampleRate = codecpar->sample_rate;
    result.selected.channels = codecpar->ch_layout.nb_channels;
    result.selected.codecProfile = codecpar->profile;
    result.selected.codecFrameSize = codecpar->frame_size;
    result.eligible = true;
    result.reason = AdtsAacSequentialReason::CompleteStrictLcAdtsInventory;
    return result;
}

AdtsAacSequentialPresentationResult probeAdtsAacSequentialPresentation(
    const std::string& path,
    const AdtsAacSelectedStreamIdentity& selected,
    const AdtsAacSequentialProbeOptions& options) {
    const auto started = Clock::now();
    AdtsAacSequentialPresentationResult result;
    result.sampleRate = selected.sampleRate;
    result.channels = selected.channels;
    result.outputDomainValidated =
        selected.codecProfile == AV_PROFILE_AAC_LOW &&
        selected.codecFrameSize == static_cast<int>(kAdtsAacSamplesPerRawDataBlock) &&
        selected.sampleRate > 0 && supportedChannelCount(selected.channels);
    if (!result.outputDomainValidated) {
        result.reason = AdtsAacSequentialReason::AacProfileUnsupported;
        return finishTimed(std::move(result), started);
    }

    std::filesystem::path nativePath;
    try {
        nativePath = std::filesystem::u8path(path);
    } catch (...) {
        result.reason = AdtsAacSequentialReason::InputNotRegularFile;
        return finishTimed(std::move(result), started);
    }
    const std::uint64_t forcedReadError = options.testHooks
        ? options.testHooks->forceReadErrorAfterBytes
        : (std::numeric_limits<std::uint64_t>::max)();
    ForwardFileReader reader(nativePath, options.readBufferBytes, forcedReadError);
    result.maximumWorkingBufferBytes = reader.maximumWorkingBufferBytes();
    if (!reader.valid()) {
        result.status = AdtsAacSequentialStatus::IoError;
        result.reason = reader.reason();
        return finishTimed(std::move(result), started);
    }
    const FileIdentity initialIdentity = reader.initialIdentity();
    result.fileSizeBytes = initialIdentity.size;
    result.fileIndex = initialIdentity.index;
    result.lastWriteTime100ns = initialIdentity.lastWriteTime;
    result.volumeSerialNumber = initialIdentity.volumeSerial;
    if (options.testHooks) {
        result.frameCount = options.testHooks->initialFrameCount;
        result.rawDataBlockCount = options.testHooks->initialRawDataBlockCount;
    }

    std::array<std::uint8_t, 7> bytes{};
    AdtsHeader first{};
    bool haveFirst = false;
    bool finished = false;
    while (!finished) {
        const ReadStatus headerRead = reader.readExact(bytes.data(), bytes.size());
        if (headerRead == ReadStatus::Eof) {
            result.reachedPhysicalEof = true;
            break;
        }
        if (headerRead == ReadStatus::Partial) {
            result.status = AdtsAacSequentialStatus::InvalidMedia;
            result.reason = AdtsAacSequentialReason::TruncatedFinalHeader;
            result.frameBoundariesValid = false;
            break;
        }
        if (headerRead == ReadStatus::Error) {
            result.status = AdtsAacSequentialStatus::IoError;
            result.reason = AdtsAacSequentialReason::ReadError;
            break;
        }

        AdtsHeader header{};
        const AdtsAacSequentialReason parsed = parseAdtsHeader(bytes, header);
        if (parsed != AdtsAacSequentialReason::CompleteStrictLcAdtsInventory) {
            result.frameBoundariesValid = false;
            if (!haveFirst) {
                result.status = AdtsAacSequentialStatus::UnsupportedEarly;
                result.reason = parsed;
            } else if (recognizedTrailingTag(bytes)) {
                result.status = AdtsAacSequentialStatus::UnsupportedLate;
                result.reason = AdtsAacSequentialReason::TrailingTagUnsupported;
            } else {
                result.status = AdtsAacSequentialStatus::InvalidMedia;
                result.reason = parsed == AdtsAacSequentialReason::InitialSyncMissing
                    ? AdtsAacSequentialReason::InvalidSyncAtFrameBoundary
                    : parsed;
            }
            break;
        }

        if (!haveFirst) {
            first = header;
            haveFirst = true;
            result.mpegId = header.mpegId;
            result.audioObjectType = header.audioObjectType;
            result.channelConfiguration = header.channelConfiguration;
            result.protectionAbsent = header.protectionAbsent;
            if (!header.protectionAbsent) {
                result.reason = AdtsAacSequentialReason::CrcProtectedUnsupported;
                finished = true;
            } else if (header.audioObjectType != 2) {
                result.reason = AdtsAacSequentialReason::AacObjectTypeUnsupported;
                finished = true;
            } else if (header.channelConfiguration == 0) {
                result.reason = AdtsAacSequentialReason::ProgramConfigElementUnsupported;
                finished = true;
            } else if (header.rawDataBlocks != 1) {
                result.reason = AdtsAacSequentialReason::MultipleRawDataBlocksUnsupported;
                finished = true;
            } else if (header.sampleRate != selected.sampleRate) {
                result.status = AdtsAacSequentialStatus::Conflict;
                result.reason = AdtsAacSequentialReason::SampleRateConflict;
                finished = true;
            } else if (channelCountForConfiguration(header.channelConfiguration) !=
                       selected.channels) {
                result.status = AdtsAacSequentialStatus::Conflict;
                result.reason = AdtsAacSequentialReason::ChannelConfigurationConflict;
                finished = true;
            }
            if (finished) {
                break;
            }
        } else if (!sameConfiguration(first, header)) {
            result.status = AdtsAacSequentialStatus::Conflict;
            result.reason = changedConfigurationReason(first, header);
            result.configurationContinuous = false;
            break;
        }

        if (result.frameCount == (std::numeric_limits<std::uint64_t>::max)() ||
            result.rawDataBlockCount >
                (std::numeric_limits<std::uint64_t>::max)() -
                    static_cast<std::uint64_t>(header.rawDataBlocks)) {
            result.status = AdtsAacSequentialStatus::InvalidMedia;
            result.reason = AdtsAacSequentialReason::CounterOverflow;
            result.checkedArithmeticValid = false;
            break;
        }
        ++result.frameCount;
        result.rawDataBlockCount += static_cast<std::uint64_t>(header.rawDataBlocks);
        result.maximumFrameBytes = (std::max<std::uint64_t>)(
            result.maximumFrameBytes, header.frameLength);

        const ReadStatus bodyRead = reader.skipExact(header.frameLength - bytes.size());
        if (bodyRead != ReadStatus::Ok) {
            result.status = bodyRead == ReadStatus::Error
                ? AdtsAacSequentialStatus::IoError
                : AdtsAacSequentialStatus::InvalidMedia;
            result.reason = bodyRead == ReadStatus::Error
                ? AdtsAacSequentialReason::ReadError
                : AdtsAacSequentialReason::TruncatedFinalPayload;
            result.frameBoundariesValid = false;
            break;
        }
    }

    FileIdentity finalIdentity;
    result.fileIdentityStable = reader.finalIdentity(finalIdentity) &&
        sameIdentity(initialIdentity, finalIdentity) &&
        !(options.testHooks && options.testHooks->forceIdentityMismatchAtEnd);
    result.bytesReturned = reader.bytesReturned();
    result.uniqueBytes = result.bytesReturned;
    result.readCalls = reader.readCalls();
    if (!result.fileIdentityStable && result.status != AdtsAacSequentialStatus::IoError) {
        result.status = AdtsAacSequentialStatus::Conflict;
        result.reason = AdtsAacSequentialReason::FileMutatedDuringScan;
    } else if (result.reachedPhysicalEof) {
        if (result.frameCount == 0 || !haveFirst) {
            result.status = AdtsAacSequentialStatus::InvalidMedia;
            result.reason = AdtsAacSequentialReason::EmptyInput;
        } else if (result.bytesReturned != result.fileSizeBytes) {
            result.status = AdtsAacSequentialStatus::Conflict;
            result.reason = AdtsAacSequentialReason::FileMutatedDuringScan;
        } else if (result.rawDataBlockCount >
                   static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) /
                       kAdtsAacSamplesPerRawDataBlock) {
            result.status = AdtsAacSequentialStatus::InvalidMedia;
            result.reason = AdtsAacSequentialReason::PresentationOverflow;
            result.checkedArithmeticValid = false;
        } else {
            result.physicalFrames =
                result.rawDataBlockCount * kAdtsAacSamplesPerRawDataBlock;
            result.presentationFrames = result.physicalFrames;
            result.status = AdtsAacSequentialStatus::Exact;
            result.reason = AdtsAacSequentialReason::CompleteStrictLcAdtsInventory;
        }
    }
    return finishTimed(std::move(result), started);
}

TotalPresentationEvidence makeAdtsAacSequentialTotalPresentationEvidence(
    const AdtsAacSequentialPresentationResult& result) noexcept {
    TotalPresentationEvidence evidence;
    if (!result.exact() || result.presentationFrames == 0 || result.sampleRate <= 0 ||
        result.presentationFrames != result.physicalFrames ||
        result.rawDataBlockCount == 0 || result.frameCount == 0 ||
        result.samplesPerRawDataBlock != kAdtsAacSamplesPerRawDataBlock ||
        result.bytesReturned != result.fileSizeBytes ||
        result.uniqueBytes != result.fileSizeBytes || result.duplicateBytes != 0 ||
        result.seekCallsAfterOpen != 0 || !result.reachedPhysicalEof ||
        !result.frameBoundariesValid || !result.configurationContinuous ||
        !result.outputDomainValidated || !result.checkedArithmeticValid ||
        !result.fileIdentityStable) {
        return evidence;
    }
    evidence.frames = result.presentationFrames;
    evidence.trust = PresentationTotalTrust::SampleExact;
    evidence.source = PresentationTotalSource::AdtsAacSequentialPresentation;
    evidence.domain = PresentationSampleDomain::NativeStreamSamples;
    evidence.sampleRate = result.sampleRate;
    evidence.exactRescale = true;
    evidence.validation = PresentationTotalValidation::SelfContainedMetadata;
    return evidence;
}

AdtsAacSequentialHandoffResult readAdtsAacSequentialPresentationHandoff(
    const std::filesystem::path& probeJsonPath,
    const std::string& sourcePath,
    const AVFormatContext* formatContext,
    const AVStream* selectedAudioStream,
    const TotalPresentationEvidence& independentEvidence) noexcept {
    AdtsAacSequentialHandoffResult handoff;
    try {
        const AdtsAacSequentialEligibility eligibility =
            evaluateAdtsAacSequentialEligibility(
                sourcePath, formatContext, selectedAudioStream, false);
        if (!eligibility.eligible) {
            handoff.reason = eligibility.reason;
            return handoff;
        }
        std::error_code fileError;
        const std::uintmax_t probeBytes =
            std::filesystem::file_size(probeJsonPath, fileError);
        if (fileError) {
            handoff.reason = AdtsAacSequentialReason::ProbeDocumentMissing;
            return handoff;
        }
        if (probeBytes == 0 || probeBytes > kMaximumProbeHandoffBytes) {
            handoff.reason = AdtsAacSequentialReason::ProbeDocumentTooLarge;
            return handoff;
        }
        std::string document;
        if (!readProbeDocument(probeJsonPath, document)) {
            handoff.reason = AdtsAacSequentialReason::ProbeDocumentInvalid;
            return handoff;
        }

        std::string recordedPath;
        std::string recordedStatus;
        std::string recordedReason;
        std::string recordedSource;
        std::uint64_t selectedStreamIndex = 0;
        std::uint64_t sampleRate = 0;
        std::uint64_t channels = 0;
        std::uint64_t mpegId = 0;
        std::uint64_t objectType = 0;
        std::uint64_t channelConfiguration = 0;
        std::uint64_t volume = 0;
        bool genericScanSkipped = false;
        auto& result = handoff.presentation;
        bool valid =
            parseJsonString(document, "sourcePath", recordedPath) &&
            parseJsonString(document, "adtsAacSequentialStatus", recordedStatus) &&
            parseJsonString(document, "adtsAacSequentialReason", recordedReason) &&
            parseJsonString(document, "decodedSampleFramesSource", recordedSource) &&
            parseJsonUnsigned(document, "selectedAudioStreamIndex", selectedStreamIndex) &&
            parseJsonUnsigned(document, "adtsAacSequentialMpegId", mpegId) &&
            parseJsonUnsigned(document, "adtsAacSequentialAudioObjectType", objectType) &&
            parseJsonUnsigned(document, "adtsAacSequentialSampleRate", sampleRate) &&
            parseJsonUnsigned(document, "adtsAacSequentialChannels", channels) &&
            parseJsonUnsigned(document, "adtsAacSequentialChannelConfiguration", channelConfiguration) &&
            parseJsonUnsigned(document, "adtsAacSequentialFrameCount", result.frameCount) &&
            parseJsonUnsigned(document, "adtsAacSequentialRawDataBlockCount", result.rawDataBlockCount) &&
            parseJsonUnsigned(document, "adtsAacSequentialSamplesPerRawDataBlock", result.samplesPerRawDataBlock) &&
            parseJsonUnsigned(document, "adtsAacSequentialPhysicalFrames", result.physicalFrames) &&
            parseJsonUnsigned(document, "adtsAacSequentialPresentationFrames", result.presentationFrames) &&
            parseJsonUnsigned(document, "adtsAacSequentialFileSizeBytes", result.fileSizeBytes) &&
            parseJsonUnsigned(document, "adtsAacSequentialFileIndex", result.fileIndex) &&
            parseJsonUnsigned(document, "adtsAacSequentialLastWriteTime100ns", result.lastWriteTime100ns) &&
            parseJsonUnsigned(document, "adtsAacSequentialVolumeSerialNumber", volume) &&
            parseJsonUnsigned(document, "adtsAacSequentialBytesReturned", result.bytesReturned) &&
            parseJsonUnsigned(document, "adtsAacSequentialUniqueBytes", result.uniqueBytes) &&
            parseJsonUnsigned(document, "adtsAacSequentialDuplicateBytes", result.duplicateBytes) &&
            parseJsonUnsigned(document, "adtsAacSequentialReadCalls", result.readCalls) &&
            parseJsonUnsigned(document, "adtsAacSequentialSeekCallsAfterOpen", result.seekCallsAfterOpen) &&
            parseJsonUnsigned(document, "adtsAacSequentialMaximumFrameBytes", result.maximumFrameBytes) &&
            parseJsonUnsigned(document, "adtsAacSequentialScanDurationUs", result.scanDurationUs) &&
            parseJsonUnsigned(document, "adtsAacSequentialMaximumWorkingBufferBytes", result.maximumWorkingBufferBytes) &&
            parseJsonBool(document, "adtsAacSequentialProtectionAbsent", result.protectionAbsent) &&
            parseJsonBool(document, "adtsAacSequentialReachedPhysicalEof", result.reachedPhysicalEof) &&
            parseJsonBool(document, "adtsAacSequentialFrameBoundariesValid", result.frameBoundariesValid) &&
            parseJsonBool(document, "adtsAacSequentialConfigurationContinuous", result.configurationContinuous) &&
            parseJsonBool(document, "adtsAacSequentialOutputDomainValidated", result.outputDomainValidated) &&
            parseJsonBool(document, "adtsAacSequentialCheckedArithmeticValid", result.checkedArithmeticValid) &&
            parseJsonBool(document, "adtsAacSequentialFileIdentityStable", result.fileIdentityStable) &&
            parseJsonBool(document, "adtsAacSequentialGenericScanSkipped", genericScanSkipped);
        if (!valid || recordedPath != sourcePath || recordedStatus != "exact" ||
            recordedReason != "complete_strict_lc_adts_inventory" ||
            recordedSource != "adts_aac_sequential_presentation" ||
            selectedStreamIndex != static_cast<std::uint64_t>(eligibility.selected.streamIndex) ||
            sampleRate != static_cast<std::uint64_t>(eligibility.selected.sampleRate) ||
            channels != static_cast<std::uint64_t>(eligibility.selected.channels) ||
            mpegId > 1 || objectType != 2 ||
            channelConfiguration != channels ||
            volume > (std::numeric_limits<std::uint32_t>::max)() ||
            !genericScanSkipped) {
            handoff.reason = AdtsAacSequentialReason::ProbeDocumentInvalid;
            return handoff;
        }
        result.mpegId = static_cast<int>(mpegId);
        result.audioObjectType = static_cast<int>(objectType);
        result.sampleRate = static_cast<int>(sampleRate);
        result.channels = static_cast<int>(channels);
        result.channelConfiguration = static_cast<int>(channelConfiguration);
        result.volumeSerialNumber = static_cast<std::uint32_t>(volume);
        result.status = AdtsAacSequentialStatus::Exact;
        result.reason = AdtsAacSequentialReason::CompleteStrictLcAdtsInventory;

        FileIdentity current;
        const FileIdentity recorded{
            result.fileSizeBytes,
            result.fileIndex,
            result.lastWriteTime100ns,
            result.volumeSerialNumber,
        };
        if (!readCurrentFileIdentity(sourcePath, current) ||
            !sameIdentity(recorded, current)) {
            handoff.status = AdtsAacSequentialHandoffStatus::Conflict;
            handoff.reason = AdtsAacSequentialReason::SourceIdentityMismatch;
            return handoff;
        }
        if (result.rawDataBlockCount == 0 ||
            result.rawDataBlockCount >
                (std::numeric_limits<std::uint64_t>::max)() /
                    kAdtsAacSamplesPerRawDataBlock ||
            result.physicalFrames !=
                result.rawDataBlockCount * kAdtsAacSamplesPerRawDataBlock ||
            result.presentationFrames != result.physicalFrames) {
            handoff.reason = AdtsAacSequentialReason::ProbeDocumentInvalid;
            return handoff;
        }
        handoff.evidence = makeAdtsAacSequentialTotalPresentationEvidence(result);
        if (handoff.evidence.trust != PresentationTotalTrust::SampleExact) {
            handoff.reason = AdtsAacSequentialReason::ProbeDocumentInvalid;
            return handoff;
        }
        if (isIndependentExact(independentEvidence) &&
            (independentEvidence.frames != handoff.evidence.frames ||
             independentEvidence.sampleRate != handoff.evidence.sampleRate)) {
            handoff.status = AdtsAacSequentialHandoffStatus::Conflict;
            handoff.reason = AdtsAacSequentialReason::ExactAuthorityConflict;
            handoff.evidence = {};
            return handoff;
        }
        handoff.status = AdtsAacSequentialHandoffStatus::Accepted;
        handoff.reason = AdtsAacSequentialReason::AcceptedFastProbeEvidence;
        return handoff;
    } catch (...) {
        handoff = {};
        handoff.reason = AdtsAacSequentialReason::ProbeDocumentInvalid;
        return handoff;
    }
}

const char* adtsAacSequentialStatusName(AdtsAacSequentialStatus status) noexcept {
    switch (status) {
        case AdtsAacSequentialStatus::Exact: return "exact";
        case AdtsAacSequentialStatus::UnsupportedEarly: return "unsupported_early";
        case AdtsAacSequentialStatus::UnsupportedLate: return "unsupported_late";
        case AdtsAacSequentialStatus::Conflict: return "conflict";
        case AdtsAacSequentialStatus::InvalidMedia: return "invalid_media";
        case AdtsAacSequentialStatus::IoError: return "io_error";
    }
    return "unsupported_early";
}

const char* adtsAacSequentialReasonName(AdtsAacSequentialReason reason) noexcept {
    switch (reason) {
        case AdtsAacSequentialReason::NotEligible: return "not_eligible";
        case AdtsAacSequentialReason::StrongerAuthorityPresent: return "stronger_authority_present";
        case AdtsAacSequentialReason::NotStandaloneAdts: return "not_standalone_adts";
        case AdtsAacSequentialReason::SelectedStreamMissing: return "selected_stream_missing";
        case AdtsAacSequentialReason::CodecNotAac: return "codec_not_aac";
        case AdtsAacSequentialReason::AacProfileUnsupported: return "aac_profile_unsupported";
        case AdtsAacSequentialReason::AacFrameSizeUnsupported: return "aac_frame_size_unsupported";
        case AdtsAacSequentialReason::OutputRateUnavailable: return "output_rate_unavailable";
        case AdtsAacSequentialReason::ChannelLayoutUnsupported: return "channel_layout_unsupported";
        case AdtsAacSequentialReason::CodecPaddingUnsupported: return "codec_padding_unsupported";
        case AdtsAacSequentialReason::InputNotRegularFile: return "input_not_regular_file";
        case AdtsAacSequentialReason::InputOpenFailed: return "input_open_failed";
        case AdtsAacSequentialReason::InputNotDiskFile: return "input_not_disk_file";
        case AdtsAacSequentialReason::FileIdentityUnavailable: return "file_identity_unavailable";
        case AdtsAacSequentialReason::EmptyInput: return "empty_input";
        case AdtsAacSequentialReason::InitialAdtsHeaderUnavailable: return "initial_adts_header_unavailable";
        case AdtsAacSequentialReason::InitialSyncMissing: return "initial_sync_missing";
        case AdtsAacSequentialReason::InvalidLayer: return "invalid_layer";
        case AdtsAacSequentialReason::InvalidSampleRateIndex: return "invalid_sample_rate_index";
        case AdtsAacSequentialReason::InvalidFrameLength: return "invalid_frame_length";
        case AdtsAacSequentialReason::CrcProtectedUnsupported: return "crc_protected_unsupported";
        case AdtsAacSequentialReason::AacObjectTypeUnsupported: return "aac_object_type_unsupported";
        case AdtsAacSequentialReason::ProgramConfigElementUnsupported: return "program_config_element_unsupported";
        case AdtsAacSequentialReason::MultipleRawDataBlocksUnsupported: return "multiple_raw_data_blocks_unsupported";
        case AdtsAacSequentialReason::SampleRateConflict: return "sample_rate_conflict";
        case AdtsAacSequentialReason::ChannelConfigurationConflict: return "channel_configuration_conflict";
        case AdtsAacSequentialReason::MpegIdChanged: return "mpeg_id_changed";
        case AdtsAacSequentialReason::AacObjectTypeChanged: return "aac_object_type_changed";
        case AdtsAacSequentialReason::SampleRateChanged: return "sample_rate_changed";
        case AdtsAacSequentialReason::ChannelConfigurationChanged: return "channel_configuration_changed";
        case AdtsAacSequentialReason::ProtectionModeChanged: return "protection_mode_changed";
        case AdtsAacSequentialReason::RawDataBlockModeChanged: return "raw_data_block_mode_changed";
        case AdtsAacSequentialReason::InvalidSyncAtFrameBoundary: return "invalid_sync_at_frame_boundary";
        case AdtsAacSequentialReason::TruncatedFinalHeader: return "truncated_final_header";
        case AdtsAacSequentialReason::TruncatedFinalPayload: return "truncated_final_payload";
        case AdtsAacSequentialReason::TrailingTagUnsupported: return "trailing_tag_unsupported";
        case AdtsAacSequentialReason::UnexpectedTrailingData: return "unexpected_trailing_data";
        case AdtsAacSequentialReason::CounterOverflow: return "counter_overflow";
        case AdtsAacSequentialReason::PresentationOverflow: return "presentation_overflow";
        case AdtsAacSequentialReason::FileMutatedDuringScan: return "file_mutated_during_scan";
        case AdtsAacSequentialReason::ReadError: return "read_error";
        case AdtsAacSequentialReason::ExactAuthorityConflict: return "exact_authority_conflict";
        case AdtsAacSequentialReason::CompleteStrictLcAdtsInventory: return "complete_strict_lc_adts_inventory";
        case AdtsAacSequentialReason::ProbeDocumentMissing: return "probe_document_missing";
        case AdtsAacSequentialReason::ProbeDocumentInvalid: return "probe_document_invalid";
        case AdtsAacSequentialReason::ProbeDocumentTooLarge: return "probe_document_too_large";
        case AdtsAacSequentialReason::SourceIdentityMismatch: return "source_identity_mismatch";
        case AdtsAacSequentialReason::AcceptedFastProbeEvidence: return "accepted_fast_probe_evidence";
    }
    return "not_eligible";
}

const char* adtsAacSequentialHandoffStatusName(
    AdtsAacSequentialHandoffStatus status) noexcept {
    switch (status) {
        case AdtsAacSequentialHandoffStatus::Accepted: return "accepted";
        case AdtsAacSequentialHandoffStatus::Unavailable: return "unavailable";
        case AdtsAacSequentialHandoffStatus::Conflict: return "conflict";
    }
    return "unavailable";
}

}  // namespace AveMediaBridge::Probe
