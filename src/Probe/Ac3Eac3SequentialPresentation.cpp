#include "Ac3Eac3SequentialPresentation.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace AveMediaBridge::Probe {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::array<std::uint32_t, 3> kDolbySampleRates{48000, 44100, 32000};
constexpr std::array<std::uint8_t, 4> kEac3Blocks{1, 2, 3, 6};
constexpr std::array<std::array<std::uint16_t, 3>, 38> kAc3FrameSizeWords{{
    {{64, 69, 96}}, {{64, 70, 96}}, {{80, 87, 120}}, {{80, 88, 120}},
    {{96, 104, 144}}, {{96, 105, 144}}, {{112, 121, 168}}, {{112, 122, 168}},
    {{128, 139, 192}}, {{128, 140, 192}}, {{160, 174, 240}}, {{160, 175, 240}},
    {{192, 208, 288}}, {{192, 209, 288}}, {{224, 243, 336}}, {{224, 244, 336}},
    {{256, 278, 384}}, {{256, 279, 384}}, {{320, 348, 480}}, {{320, 349, 480}},
    {{384, 417, 576}}, {{384, 418, 576}}, {{448, 487, 672}}, {{448, 488, 672}},
    {{512, 557, 768}}, {{512, 558, 768}}, {{640, 696, 960}}, {{640, 697, 960}},
    {{768, 835, 1152}}, {{768, 836, 1152}}, {{896, 975, 1344}}, {{896, 976, 1344}},
    {{1024, 1114, 1536}}, {{1024, 1115, 1536}}, {{1152, 1253, 1728}},
    {{1152, 1254, 1728}}, {{1280, 1393, 1920}}, {{1280, 1394, 1920}},
}};
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
          buffer_((std::max)(bufferBytes, std::size_t{16})),
          forceReadErrorAfterBytes_(forceReadErrorAfterBytes) {
        if (!handle_.valid()) {
            reason_ = DolbySequentialPresentationReason::InputOpenFailed;
            return;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (GetFileType(handle_.get()) != FILE_TYPE_DISK ||
            !GetFileInformationByHandle(handle_.get(), &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            reason_ = DolbySequentialPresentationReason::InputNotDiskFile;
            return;
        }
        initialIdentity_ = identityFrom(info);
        valid_ = true;
    }

    bool valid() const noexcept { return valid_; }
    DolbySequentialPresentationReason reason() const noexcept { return reason_; }
    const FileIdentity& initialIdentity() const noexcept { return initialIdentity_; }
    std::uint64_t bytesReturned() const noexcept { return bytesReturned_; }
    std::uint64_t readCalls() const noexcept { return readCalls_; }
    std::uint64_t consumed() const noexcept { return consumed_; }
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
            consumed_ += count;
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
            consumed_ += count;
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
    std::uint64_t consumed_ = 0;
    std::uint64_t readCalls_ = 0;
    std::uint64_t forceReadErrorAfterBytes_ =
        (std::numeric_limits<std::uint64_t>::max)();
    bool valid_ = false;
    DolbySequentialPresentationReason reason_ =
        DolbySequentialPresentationReason::FileIdentityUnavailable;
};

struct ParsedHeader {
    DolbySequentialCodecFamily family = DolbySequentialCodecFamily::Unknown;
    std::uint32_t frameSizeBytes = 0;
    std::uint32_t sampleRate = 0;
    std::uint8_t bitstreamId = 0;
    std::uint8_t channelMode = 0;
    bool lfe = false;
    std::uint8_t audioBlocks = 0;
    std::uint8_t streamType = 0;
    std::uint8_t substreamId = 0;
};

std::uint64_t headerWord(const std::array<std::uint8_t, 7>& bytes) noexcept {
    std::uint64_t word = 0;
    for (std::uint8_t byte : bytes) {
        word = (word << 8) | byte;
    }
    return word;
}

std::uint32_t field(
    std::uint64_t word,
    unsigned firstBit,
    unsigned count) noexcept {
    const unsigned right = 56U - firstBit - count;
    return static_cast<std::uint32_t>(
        (word >> right) & ((std::uint64_t{1} << count) - 1U));
}

DolbySequentialPresentationReason parseHeader(
    const std::array<std::uint8_t, 7>& bytes,
    ParsedHeader& header) noexcept {
    header = {};
    const std::uint64_t word = headerWord(bytes);
    if (field(word, 0, 16) != 0x0b77) {
        return DolbySequentialPresentationReason::InitialSyncMissing;
    }
    header.bitstreamId = static_cast<std::uint8_t>(field(word, 40, 5));
    if (header.bitstreamId <= 10) {
        header.family = DolbySequentialCodecFamily::Ac3;
        const std::uint8_t sampleRateCode =
            static_cast<std::uint8_t>(field(word, 32, 2));
        const std::uint8_t frameSizeCode =
            static_cast<std::uint8_t>(field(word, 34, 6));
        if (sampleRateCode >= kDolbySampleRates.size() ||
            frameSizeCode >= kAc3FrameSizeWords.size()) {
            return DolbySequentialPresentationReason::InvalidHeader;
        }
        if (header.bitstreamId != 8) {
            return DolbySequentialPresentationReason::UnsupportedBitstreamId;
        }
        header.sampleRate = kDolbySampleRates[sampleRateCode];
        header.frameSizeBytes =
            static_cast<std::uint32_t>(
                kAc3FrameSizeWords[frameSizeCode][sampleRateCode]) * 2U;
        header.channelMode = static_cast<std::uint8_t>(field(word, 48, 3));
        unsigned lfeBit = 51;
        if (header.channelMode == 2) {
            lfeBit += 2;
        } else {
            if ((header.channelMode & 1U) && header.channelMode != 1) {
                lfeBit += 2;
            }
            if (header.channelMode & 4U) {
                lfeBit += 2;
            }
        }
        if (lfeBit >= 56 || header.frameSizeBytes < bytes.size()) {
            return DolbySequentialPresentationReason::InvalidHeader;
        }
        header.lfe = field(word, lfeBit, 1) != 0;
        header.audioBlocks = 6;
        return DolbySequentialPresentationReason::CompleteStrictAc3Inventory;
    }

    header.family = DolbySequentialCodecFamily::Eac3;
    if (header.bitstreamId < 11 || header.bitstreamId > 16) {
        return DolbySequentialPresentationReason::UnsupportedBitstreamId;
    }
    header.streamType = static_cast<std::uint8_t>(field(word, 16, 2));
    header.substreamId = static_cast<std::uint8_t>(field(word, 18, 3));
    header.frameSizeBytes = (field(word, 21, 11) + 1U) * 2U;
    const std::uint8_t sampleRateCode =
        static_cast<std::uint8_t>(field(word, 32, 2));
    if (header.streamType == 3 || header.frameSizeBytes < bytes.size()) {
        return DolbySequentialPresentationReason::InvalidHeader;
    }
    if (sampleRateCode == 3) {
        return DolbySequentialPresentationReason::UnsupportedReducedRateEac3;
    }
    header.sampleRate = kDolbySampleRates[sampleRateCode];
    header.audioBlocks = kEac3Blocks[field(word, 34, 2)];
    header.channelMode = static_cast<std::uint8_t>(field(word, 36, 3));
    header.lfe = field(word, 39, 1) != 0;
    if (header.streamType != 0) {
        return DolbySequentialPresentationReason::UnsupportedEac3StreamType;
    }
    if (header.substreamId != 0) {
        return DolbySequentialPresentationReason::UnsupportedEac3Substream;
    }
    return DolbySequentialPresentationReason::CompleteStrictEac3Inventory;
}

int channelCount(std::uint8_t channelMode, bool lfe) noexcept {
    constexpr std::array<int, 8> kBaseChannels{2, 1, 2, 3, 3, 4, 4, 5};
    return kBaseChannels[channelMode] + (lfe ? 1 : 0);
}

bool checkedAdd(std::uint64_t& value, std::uint64_t amount) noexcept {
    if (amount > (std::numeric_limits<std::uint64_t>::max)() - value) {
        return false;
    }
    value += amount;
    return true;
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

DolbySequentialPresentationResult finishTimed(
    DolbySequentialPresentationResult result,
    Clock::time_point started) noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - started).count();
    result.scanDurationUs = elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
    return result;
}

DolbySequentialPresentationStatus unsupportedStatus(
    std::uint64_t syncframes) noexcept {
    return syncframes == 0
        ? DolbySequentialPresentationStatus::UnsupportedEarly
        : DolbySequentialPresentationStatus::UnsupportedLate;
}

bool expectedFamilyMatchesCodec(
    DolbySequentialCodecFamily family,
    AVCodecID codecId) noexcept {
    return (family == DolbySequentialCodecFamily::Ac3 && codecId == AV_CODEC_ID_AC3) ||
        (family == DolbySequentialCodecFamily::Eac3 && codecId == AV_CODEC_ID_EAC3);
}

}  // namespace

DolbySequentialEligibility evaluateDolbySequentialEligibility(
    const std::string& path,
    const AVFormatContext* formatContext,
    const AVStream* selectedAudioStream,
    bool strongerSampleExactAuthorityPresent) {
    DolbySequentialEligibility result;
    if (strongerSampleExactAuthorityPresent) {
        result.reason = DolbySequentialPresentationReason::StrongerAuthorityPresent;
        return result;
    }
    if (path.empty() || !formatContext || !formatContext->iformat ||
        !formatContext->iformat->name) {
        result.reason = DolbySequentialPresentationReason::NotStandaloneRawDolby;
        return result;
    }
    const std::string_view formatName(formatContext->iformat->name);
    if (formatName != "ac3" && formatName != "eac3") {
        result.reason = DolbySequentialPresentationReason::NotStandaloneRawDolby;
        return result;
    }
    if (!selectedAudioStream || !selectedAudioStream->codecpar ||
        selectedAudioStream->index < 0) {
        result.reason = DolbySequentialPresentationReason::SelectedStreamMissing;
        return result;
    }
    const AVCodecParameters* codecpar = selectedAudioStream->codecpar;
    const bool codecMatches = codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
        ((formatName == "ac3" && codecpar->codec_id == AV_CODEC_ID_AC3) ||
         (formatName == "eac3" && codecpar->codec_id == AV_CODEC_ID_EAC3));
    if (!codecMatches) {
        result.reason = DolbySequentialPresentationReason::CodecMismatch;
        return result;
    }
    if (codecpar->sample_rate != 32000 && codecpar->sample_rate != 44100 &&
        codecpar->sample_rate != 48000) {
        result.reason = DolbySequentialPresentationReason::OutputRateUnavailable;
        return result;
    }
    if (codecpar->ch_layout.nb_channels <= 0 ||
        !av_channel_layout_check(&codecpar->ch_layout)) {
        result.reason = DolbySequentialPresentationReason::ChannelLayoutUnsupported;
        return result;
    }
    if (codecpar->initial_padding != 0 || codecpar->trailing_padding != 0) {
        result.reason = DolbySequentialPresentationReason::CodecPaddingUnsupported;
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
        result.reason = DolbySequentialPresentationReason::SelectedStreamMissing;
        return result;
    }
    try {
        std::error_code error;
        const std::filesystem::path nativePath = std::filesystem::u8path(path);
        if (!std::filesystem::is_regular_file(nativePath, error) || error) {
            result.reason = DolbySequentialPresentationReason::InputNotRegularFile;
            return result;
        }
    } catch (...) {
        result.reason = DolbySequentialPresentationReason::InputNotRegularFile;
        return result;
    }

    result.selected.streamIndex = static_cast<int>(selectedAudioStream->index);
    result.selected.codecId = codecpar->codec_id;
    result.selected.sampleRate = codecpar->sample_rate;
    result.selected.channels = codecpar->ch_layout.nb_channels;
    result.eligible = true;
    result.reason = formatName == "ac3"
        ? DolbySequentialPresentationReason::CompleteStrictAc3Inventory
        : DolbySequentialPresentationReason::CompleteStrictEac3Inventory;
    return result;
}

DolbySequentialPresentationResult probeDolbySequentialPresentation(
    const std::string& path,
    const DolbySelectedStreamIdentity& selected,
    const DolbySequentialProbeOptions& options) {
    const auto started = Clock::now();
    DolbySequentialPresentationResult result;
    result.sampleRate = selected.sampleRate;
    result.channels = selected.channels;
    result.outputDomainValidated =
        (selected.codecId == AV_CODEC_ID_AC3 || selected.codecId == AV_CODEC_ID_EAC3) &&
        (selected.sampleRate == 32000 || selected.sampleRate == 44100 ||
         selected.sampleRate == 48000) && selected.channels > 0;
    if (!result.outputDomainValidated) {
        result.reason = DolbySequentialPresentationReason::CodecMismatch;
        return finishTimed(std::move(result), started);
    }

    std::filesystem::path nativePath;
    try {
        nativePath = std::filesystem::u8path(path);
    } catch (...) {
        result.reason = DolbySequentialPresentationReason::InputNotRegularFile;
        return finishTimed(std::move(result), started);
    }
    const std::uint64_t forcedReadError = options.testHooks
        ? options.testHooks->forceReadErrorAfterBytes
        : (std::numeric_limits<std::uint64_t>::max)();
    ForwardFileReader reader(nativePath, options.readBufferBytes, forcedReadError);
    result.maximumWorkingBufferBytes = reader.maximumWorkingBufferBytes();
    if (!reader.valid()) {
        result.status = DolbySequentialPresentationStatus::IoError;
        result.reason = reader.reason();
        return finishTimed(std::move(result), started);
    }
    const FileIdentity initialIdentity = reader.initialIdentity();
    result.fileSizeBytes = initialIdentity.size;
    result.fileIndex = initialIdentity.index;
    result.lastWriteTime100ns = initialIdentity.lastWriteTime;
    result.volumeSerialNumber = initialIdentity.volumeSerial;
    if (options.testHooks) {
        result.syncframeCount = options.testHooks->initialSyncframeCount;
        result.audioBlockCount = options.testHooks->initialAudioBlockCount;
    }

    ParsedHeader first{};
    bool haveFirst = false;
    std::array<std::uint8_t, 7> bytes{};
    while (true) {
        const ReadStatus headerRead = reader.readExact(bytes.data(), bytes.size());
        if (headerRead == ReadStatus::Eof) {
            result.reachedPhysicalEof = true;
            break;
        }
        if (headerRead == ReadStatus::Partial) {
            result.status = DolbySequentialPresentationStatus::InvalidMedia;
            result.reason = DolbySequentialPresentationReason::TruncatedFinalHeader;
            result.frameBoundariesValid = false;
            break;
        }
        if (headerRead == ReadStatus::Error) {
            result.status = DolbySequentialPresentationStatus::IoError;
            result.reason = DolbySequentialPresentationReason::ReadError;
            break;
        }

        ParsedHeader header{};
        const DolbySequentialPresentationReason parsed = parseHeader(bytes, header);
        const bool parsedExact =
            parsed == DolbySequentialPresentationReason::CompleteStrictAc3Inventory ||
            parsed == DolbySequentialPresentationReason::CompleteStrictEac3Inventory;
        if (!parsedExact) {
            result.frameBoundariesValid = false;
            if (parsed == DolbySequentialPresentationReason::UnsupportedBitstreamId ||
                parsed == DolbySequentialPresentationReason::UnsupportedReducedRateEac3 ||
                parsed == DolbySequentialPresentationReason::UnsupportedEac3StreamType ||
                parsed == DolbySequentialPresentationReason::UnsupportedEac3Substream) {
                if (parsed == DolbySequentialPresentationReason::UnsupportedEac3StreamType &&
                    field(headerWord(bytes), 16, 2) == 1) {
                    ++result.eac3DependentFrameCount;
                }
                if (parsed == DolbySequentialPresentationReason::UnsupportedEac3StreamType ||
                    parsed == DolbySequentialPresentationReason::UnsupportedEac3Substream) {
                    result.substreamPolicyValid = false;
                }
                result.status = unsupportedStatus(result.syncframeCount);
                result.reason = parsed;
            } else if (!haveFirst &&
                parsed == DolbySequentialPresentationReason::InitialSyncMissing) {
                result.status = DolbySequentialPresentationStatus::UnsupportedEarly;
                result.reason = parsed;
            } else {
                result.status = DolbySequentialPresentationStatus::InvalidMedia;
                result.reason = parsed == DolbySequentialPresentationReason::InitialSyncMissing
                    ? DolbySequentialPresentationReason::InvalidSyncAtFrameBoundary
                    : parsed;
            }
            break;
        }

        result.crcObserved = true;

        if (!haveFirst) {
            first = header;
            haveFirst = true;
            result.family = header.family;
            result.bitstreamId = header.bitstreamId;
            result.channelMode = header.channelMode;
            result.lfe = header.lfe;
            result.selectedStreamType = header.streamType;
            result.selectedSubstreamId = header.substreamId;
            if (!expectedFamilyMatchesCodec(header.family, selected.codecId)) {
                result.status = DolbySequentialPresentationStatus::Conflict;
                result.reason = DolbySequentialPresentationReason::CodecMismatch;
                break;
            }
            if (header.sampleRate != static_cast<std::uint32_t>(selected.sampleRate)) {
                result.status = DolbySequentialPresentationStatus::Conflict;
                result.reason = DolbySequentialPresentationReason::SampleRateConflict;
                break;
            }
            if (channelCount(header.channelMode, header.lfe) != selected.channels) {
                result.status = DolbySequentialPresentationStatus::Conflict;
                result.reason = DolbySequentialPresentationReason::ChannelConfigurationConflict;
                break;
            }
        } else {
            if (header.family != first.family) {
                result.status = DolbySequentialPresentationStatus::Conflict;
                result.reason = DolbySequentialPresentationReason::MixedCodecFamily;
                result.configurationContinuous = false;
                break;
            }
            if (header.sampleRate != first.sampleRate) {
                result.status = DolbySequentialPresentationStatus::Conflict;
                result.reason = DolbySequentialPresentationReason::SampleRateChanged;
                result.configurationContinuous = false;
                break;
            }
            if (header.channelMode != first.channelMode || header.lfe != first.lfe) {
                result.status = DolbySequentialPresentationStatus::Conflict;
                result.reason = DolbySequentialPresentationReason::ChannelConfigurationChanged;
                result.configurationContinuous = false;
                break;
            }
            if (header.bitstreamId != first.bitstreamId) {
                result.status = DolbySequentialPresentationStatus::Conflict;
                result.reason = DolbySequentialPresentationReason::BitstreamIdChanged;
                result.configurationContinuous = false;
                break;
            }
        }

        const ReadStatus frameRead = reader.skipExact(header.frameSizeBytes - bytes.size());
        if (frameRead != ReadStatus::Ok) {
            result.status = frameRead == ReadStatus::Error
                ? DolbySequentialPresentationStatus::IoError
                : DolbySequentialPresentationStatus::InvalidMedia;
            result.reason = frameRead == ReadStatus::Error
                ? DolbySequentialPresentationReason::ReadError
                : DolbySequentialPresentationReason::TruncatedFinalFrame;
            result.frameBoundariesValid = false;
            break;
        }
        result.maximumFrameBytes = (std::max<std::uint64_t>)(
            result.maximumFrameBytes, header.frameSizeBytes);
        const std::uint64_t frameSamples =
            static_cast<std::uint64_t>(header.audioBlocks) *
            kEac3SamplesPerAudioBlock;
        if (!checkedAdd(result.syncframeCount, 1) ||
            !checkedAdd(result.audioBlockCount, header.audioBlocks) ||
            !checkedAdd(result.presentationFrames, frameSamples)) {
            result.status = DolbySequentialPresentationStatus::InvalidMedia;
            result.reason = DolbySequentialPresentationReason::CounterOverflow;
            result.checkedArithmeticValid = false;
            break;
        }
        if (header.family == DolbySequentialCodecFamily::Ac3) {
            if (!checkedAdd(result.ac3FrameCount, 1)) {
                result.status = DolbySequentialPresentationStatus::InvalidMedia;
                result.reason = DolbySequentialPresentationReason::CounterOverflow;
                result.checkedArithmeticValid = false;
                break;
            }
        } else if (!checkedAdd(result.eac3IndependentFrameCount, 1)) {
            result.status = DolbySequentialPresentationStatus::InvalidMedia;
            result.reason = DolbySequentialPresentationReason::CounterOverflow;
            result.checkedArithmeticValid = false;
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
    if (!result.fileIdentityStable &&
        result.status != DolbySequentialPresentationStatus::IoError) {
        result.status = DolbySequentialPresentationStatus::Conflict;
        result.reason = DolbySequentialPresentationReason::FileMutatedDuringScan;
    } else if (result.reachedPhysicalEof) {
        if (!haveFirst || result.syncframeCount == 0) {
            result.status = DolbySequentialPresentationStatus::InvalidMedia;
            result.reason = DolbySequentialPresentationReason::EmptyInput;
        } else if (reader.consumed() != result.fileSizeBytes ||
            result.bytesReturned != result.fileSizeBytes) {
            result.status = DolbySequentialPresentationStatus::Conflict;
            result.reason = DolbySequentialPresentationReason::FileMutatedDuringScan;
        } else if (result.audioBlockCount >
            static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) /
                kEac3SamplesPerAudioBlock) {
            result.status = DolbySequentialPresentationStatus::InvalidMedia;
            result.reason = DolbySequentialPresentationReason::PresentationOverflow;
            result.checkedArithmeticValid = false;
        } else {
            const std::uint64_t expectedPresentation =
                result.audioBlockCount * kEac3SamplesPerAudioBlock;
            if (expectedPresentation != result.presentationFrames) {
                result.status = DolbySequentialPresentationStatus::InvalidMedia;
                result.reason = DolbySequentialPresentationReason::PresentationOverflow;
                result.checkedArithmeticValid = false;
            } else if (result.family == DolbySequentialCodecFamily::Ac3 &&
                (result.ac3FrameCount != result.syncframeCount ||
                 result.audioBlockCount != result.syncframeCount * 6U)) {
                result.status = DolbySequentialPresentationStatus::InvalidMedia;
                result.reason = DolbySequentialPresentationReason::PresentationOverflow;
                result.checkedArithmeticValid = false;
            } else {
                result.status = result.family == DolbySequentialCodecFamily::Ac3
                    ? DolbySequentialPresentationStatus::ExactAc3
                    : DolbySequentialPresentationStatus::ExactEac3;
                result.reason = result.family == DolbySequentialCodecFamily::Ac3
                    ? DolbySequentialPresentationReason::CompleteStrictAc3Inventory
                    : DolbySequentialPresentationReason::CompleteStrictEac3Inventory;
            }
        }
    }
    return finishTimed(std::move(result), started);
}

TotalPresentationEvidence makeDolbySequentialTotalPresentationEvidence(
    const DolbySequentialPresentationResult& result) noexcept {
    TotalPresentationEvidence evidence;
    if (!result.exact() || result.presentationFrames == 0 ||
        result.sampleRate <= 0 || result.syncframeCount == 0 ||
        result.audioBlockCount == 0 ||
        result.samplesPerAudioBlock != kEac3SamplesPerAudioBlock ||
        result.bytesReturned != result.fileSizeBytes ||
        result.uniqueBytes != result.fileSizeBytes || result.duplicateBytes != 0 ||
        result.seekCallsAfterOpen != 0 || !result.reachedPhysicalEof ||
        !result.frameBoundariesValid || !result.configurationContinuous ||
        !result.substreamPolicyValid || !result.outputDomainValidated ||
        !result.checkedArithmeticValid || !result.fileIdentityStable) {
        return evidence;
    }
    if (result.family == DolbySequentialCodecFamily::Ac3 &&
        (result.syncframeCount >
             (std::numeric_limits<std::uint64_t>::max)() /
                 kAc3SamplesPerSyncframe ||
         result.ac3FrameCount != result.syncframeCount ||
         result.presentationFrames != result.syncframeCount * kAc3SamplesPerSyncframe)) {
        return evidence;
    }
    if (result.family == DolbySequentialCodecFamily::Eac3 &&
        (result.audioBlockCount >
             (std::numeric_limits<std::uint64_t>::max)() /
                 kEac3SamplesPerAudioBlock ||
         result.eac3IndependentFrameCount != result.syncframeCount ||
         result.eac3DependentFrameCount != 0 || result.selectedStreamType != 0 ||
         result.selectedSubstreamId != 0 ||
         result.presentationFrames !=
            result.audioBlockCount * kEac3SamplesPerAudioBlock)) {
        return evidence;
    }
    evidence.frames = result.presentationFrames;
    evidence.trust = PresentationTotalTrust::SampleExact;
    evidence.source = result.family == DolbySequentialCodecFamily::Ac3
        ? PresentationTotalSource::Ac3SequentialPresentation
        : PresentationTotalSource::Eac3SequentialPresentation;
    evidence.domain = PresentationSampleDomain::NativeStreamSamples;
    evidence.sampleRate = result.sampleRate;
    evidence.exactRescale = true;
    evidence.validation = PresentationTotalValidation::SelfContainedMetadata;
    return evidence;
}

DolbySequentialHandoffResult readDolbySequentialPresentationHandoff(
    const std::filesystem::path& probeJsonPath,
    const std::string& sourcePath,
    const AVFormatContext* formatContext,
    const AVStream* selectedAudioStream,
    const TotalPresentationEvidence& independentEvidence) noexcept {
    DolbySequentialHandoffResult handoff;
    try {
        const DolbySequentialEligibility eligibility =
            evaluateDolbySequentialEligibility(
                sourcePath, formatContext, selectedAudioStream, false);
        if (!eligibility.eligible) {
            handoff.reason = eligibility.reason;
            return handoff;
        }
        std::error_code fileError;
        const std::uintmax_t probeBytes =
            std::filesystem::file_size(probeJsonPath, fileError);
        if (fileError) {
            handoff.reason = DolbySequentialPresentationReason::ProbeDocumentMissing;
            return handoff;
        }
        if (probeBytes == 0 || probeBytes > kMaximumProbeHandoffBytes) {
            handoff.reason = DolbySequentialPresentationReason::ProbeDocumentTooLarge;
            return handoff;
        }
        std::string document;
        if (!readProbeDocument(probeJsonPath, document)) {
            handoff.reason = DolbySequentialPresentationReason::ProbeDocumentInvalid;
            return handoff;
        }

        std::string recordedPath;
        std::string recordedStatus;
        std::string recordedReason;
        std::string recordedSource;
        std::string recordedFamily;
        std::uint64_t selectedStreamIndex = 0;
        std::uint64_t sampleRate = 0;
        std::uint64_t channels = 0;
        std::uint64_t bitstreamId = 0;
        std::uint64_t channelMode = 0;
        std::uint64_t streamType = 0;
        std::uint64_t substreamId = 0;
        std::uint64_t volume = 0;
        bool lfe = false;
        auto& result = handoff.presentation;
        const bool valid =
            parseJsonString(document, "sourcePath", recordedPath) &&
            parseJsonString(document, "dolbySequentialStatus", recordedStatus) &&
            parseJsonString(document, "dolbySequentialReason", recordedReason) &&
            parseJsonString(document, "dolbySequentialCodecFamily", recordedFamily) &&
            parseJsonString(document, "decodedSampleFramesSource", recordedSource) &&
            parseJsonUnsigned(document, "selectedAudioStreamIndex", selectedStreamIndex) &&
            parseJsonUnsigned(document, "dolbySequentialSampleRate", sampleRate) &&
            parseJsonUnsigned(document, "dolbySequentialChannels", channels) &&
            parseJsonUnsigned(document, "dolbySequentialBitstreamId", bitstreamId) &&
            parseJsonUnsigned(document, "dolbySequentialChannelMode", channelMode) &&
            parseJsonUnsigned(document, "dolbySequentialSelectedStreamType", streamType) &&
            parseJsonUnsigned(document, "dolbySequentialSelectedSubstreamId", substreamId) &&
            parseJsonUnsigned(document, "dolbySequentialSyncframeCount", result.syncframeCount) &&
            parseJsonUnsigned(document, "dolbySequentialAc3FrameCount", result.ac3FrameCount) &&
            parseJsonUnsigned(document, "dolbySequentialEac3IndependentFrameCount", result.eac3IndependentFrameCount) &&
            parseJsonUnsigned(document, "dolbySequentialEac3DependentFrameCount", result.eac3DependentFrameCount) &&
            parseJsonUnsigned(document, "dolbySequentialAudioBlockCount", result.audioBlockCount) &&
            parseJsonUnsigned(document, "dolbySequentialSamplesPerAudioBlock", result.samplesPerAudioBlock) &&
            parseJsonUnsigned(document, "dolbySequentialPresentationFrames", result.presentationFrames) &&
            parseJsonUnsigned(document, "dolbySequentialFileSizeBytes", result.fileSizeBytes) &&
            parseJsonUnsigned(document, "dolbySequentialFileIndex", result.fileIndex) &&
            parseJsonUnsigned(document, "dolbySequentialLastWriteTime100ns", result.lastWriteTime100ns) &&
            parseJsonUnsigned(document, "dolbySequentialVolumeSerialNumber", volume) &&
            parseJsonUnsigned(document, "dolbySequentialBytesReturned", result.bytesReturned) &&
            parseJsonUnsigned(document, "dolbySequentialUniqueBytes", result.uniqueBytes) &&
            parseJsonUnsigned(document, "dolbySequentialDuplicateBytes", result.duplicateBytes) &&
            parseJsonUnsigned(document, "dolbySequentialReadCalls", result.readCalls) &&
            parseJsonUnsigned(document, "dolbySequentialSeekCallsAfterOpen", result.seekCallsAfterOpen) &&
            parseJsonUnsigned(document, "dolbySequentialMaximumFrameBytes", result.maximumFrameBytes) &&
            parseJsonUnsigned(document, "dolbySequentialScanDurationUs", result.scanDurationUs) &&
            parseJsonUnsigned(document, "dolbySequentialMaximumWorkingBufferBytes", result.maximumWorkingBufferBytes) &&
            parseJsonBool(document, "dolbySequentialLfe", lfe) &&
            parseJsonBool(document, "dolbySequentialReachedPhysicalEof", result.reachedPhysicalEof) &&
            parseJsonBool(document, "dolbySequentialFrameBoundariesValid", result.frameBoundariesValid) &&
            parseJsonBool(document, "dolbySequentialConfigurationContinuous", result.configurationContinuous) &&
            parseJsonBool(document, "dolbySequentialSubstreamPolicyValid", result.substreamPolicyValid) &&
            parseJsonBool(document, "dolbySequentialOutputDomainValidated", result.outputDomainValidated) &&
            parseJsonBool(document, "dolbySequentialCheckedArithmeticValid", result.checkedArithmeticValid) &&
            parseJsonBool(document, "dolbySequentialFileIdentityStable", result.fileIdentityStable);
        const DolbySequentialCodecFamily family = recordedFamily == "ac3"
            ? DolbySequentialCodecFamily::Ac3
            : recordedFamily == "eac3"
                ? DolbySequentialCodecFamily::Eac3
                : DolbySequentialCodecFamily::Unknown;
        const std::string expectedStatus = family == DolbySequentialCodecFamily::Ac3
            ? "exact_ac3" : "exact_eac3";
        const std::string expectedReason = family == DolbySequentialCodecFamily::Ac3
            ? "complete_strict_ac3_inventory"
            : "complete_strict_eac3_inventory";
        const std::string expectedSource = family == DolbySequentialCodecFamily::Ac3
            ? "ac3_sequential_presentation"
            : "eac3_sequential_presentation";
        if (!valid || recordedPath != sourcePath || family == DolbySequentialCodecFamily::Unknown ||
            recordedStatus != expectedStatus || recordedReason != expectedReason ||
            recordedSource != expectedSource ||
            selectedStreamIndex != static_cast<std::uint64_t>(eligibility.selected.streamIndex) ||
            sampleRate != static_cast<std::uint64_t>(eligibility.selected.sampleRate) ||
            channels != static_cast<std::uint64_t>(eligibility.selected.channels) ||
            !expectedFamilyMatchesCodec(family, eligibility.selected.codecId) ||
            bitstreamId > 16 || channelMode > 7 || streamType > 3 ||
            substreamId > 7 || volume > (std::numeric_limits<std::uint32_t>::max)()) {
            handoff.reason = DolbySequentialPresentationReason::ProbeDocumentInvalid;
            return handoff;
        }

        result.family = family;
        result.sampleRate = static_cast<int>(sampleRate);
        result.channels = static_cast<int>(channels);
        result.bitstreamId = static_cast<int>(bitstreamId);
        result.channelMode = static_cast<int>(channelMode);
        result.lfe = lfe;
        result.selectedStreamType = family == DolbySequentialCodecFamily::Eac3
            ? static_cast<int>(streamType) : -1;
        result.selectedSubstreamId = family == DolbySequentialCodecFamily::Eac3
            ? static_cast<int>(substreamId) : -1;
        result.volumeSerialNumber = static_cast<std::uint32_t>(volume);
        result.status = family == DolbySequentialCodecFamily::Ac3
            ? DolbySequentialPresentationStatus::ExactAc3
            : DolbySequentialPresentationStatus::ExactEac3;
        result.reason = family == DolbySequentialCodecFamily::Ac3
            ? DolbySequentialPresentationReason::CompleteStrictAc3Inventory
            : DolbySequentialPresentationReason::CompleteStrictEac3Inventory;
        result.crcObserved = true;

        FileIdentity current;
        const FileIdentity recorded{
            result.fileSizeBytes,
            result.fileIndex,
            result.lastWriteTime100ns,
            result.volumeSerialNumber,
        };
        if (!readCurrentFileIdentity(sourcePath, current) ||
            !sameIdentity(recorded, current)) {
            handoff.status = DolbySequentialHandoffStatus::Conflict;
            handoff.reason = DolbySequentialPresentationReason::SourceIdentityMismatch;
            return handoff;
        }
        handoff.evidence = makeDolbySequentialTotalPresentationEvidence(result);
        if (handoff.evidence.trust != PresentationTotalTrust::SampleExact) {
            handoff.reason = DolbySequentialPresentationReason::ProbeDocumentInvalid;
            return handoff;
        }
        if (isIndependentExact(independentEvidence) &&
            (independentEvidence.frames != handoff.evidence.frames ||
             independentEvidence.sampleRate != handoff.evidence.sampleRate)) {
            handoff.status = DolbySequentialHandoffStatus::Conflict;
            handoff.reason = DolbySequentialPresentationReason::ExactAuthorityConflict;
            handoff.evidence = {};
            return handoff;
        }
        handoff.status = DolbySequentialHandoffStatus::Accepted;
        handoff.reason = DolbySequentialPresentationReason::AcceptedFastProbeEvidence;
        return handoff;
    } catch (...) {
        handoff = {};
        handoff.reason = DolbySequentialPresentationReason::ProbeDocumentInvalid;
        return handoff;
    }
}

const char* dolbySequentialCodecFamilyName(
    DolbySequentialCodecFamily family) noexcept {
    switch (family) {
        case DolbySequentialCodecFamily::Ac3: return "ac3";
        case DolbySequentialCodecFamily::Eac3: return "eac3";
        case DolbySequentialCodecFamily::Unknown: return "unknown";
    }
    return "unknown";
}

const char* dolbySequentialStatusName(
    DolbySequentialPresentationStatus status) noexcept {
    switch (status) {
        case DolbySequentialPresentationStatus::ExactAc3: return "exact_ac3";
        case DolbySequentialPresentationStatus::ExactEac3: return "exact_eac3";
        case DolbySequentialPresentationStatus::UnsupportedEarly: return "unsupported_early";
        case DolbySequentialPresentationStatus::UnsupportedLate: return "unsupported_late";
        case DolbySequentialPresentationStatus::Conflict: return "conflict";
        case DolbySequentialPresentationStatus::InvalidMedia: return "invalid_media";
        case DolbySequentialPresentationStatus::IoError: return "io_error";
    }
    return "unsupported_early";
}

const char* dolbySequentialReasonName(
    DolbySequentialPresentationReason reason) noexcept {
    switch (reason) {
        case DolbySequentialPresentationReason::NotEligible: return "not_eligible";
        case DolbySequentialPresentationReason::StrongerAuthorityPresent: return "stronger_authority_present";
        case DolbySequentialPresentationReason::NotStandaloneRawDolby: return "not_standalone_raw_dolby";
        case DolbySequentialPresentationReason::SelectedStreamMissing: return "selected_stream_missing";
        case DolbySequentialPresentationReason::CodecMismatch: return "codec_mismatch";
        case DolbySequentialPresentationReason::OutputRateUnavailable: return "output_rate_unavailable";
        case DolbySequentialPresentationReason::ChannelLayoutUnsupported: return "channel_layout_unsupported";
        case DolbySequentialPresentationReason::CodecPaddingUnsupported: return "codec_padding_unsupported";
        case DolbySequentialPresentationReason::InputNotRegularFile: return "input_not_regular_file";
        case DolbySequentialPresentationReason::InputOpenFailed: return "input_open_failed";
        case DolbySequentialPresentationReason::InputNotDiskFile: return "input_not_disk_file";
        case DolbySequentialPresentationReason::FileIdentityUnavailable: return "file_identity_unavailable";
        case DolbySequentialPresentationReason::EmptyInput: return "empty_input";
        case DolbySequentialPresentationReason::InitialHeaderUnavailable: return "initial_header_unavailable";
        case DolbySequentialPresentationReason::InitialSyncMissing: return "initial_sync_missing";
        case DolbySequentialPresentationReason::InvalidHeader: return "invalid_header";
        case DolbySequentialPresentationReason::InvalidSyncAtFrameBoundary: return "invalid_sync_at_frame_boundary";
        case DolbySequentialPresentationReason::TruncatedFinalHeader: return "truncated_final_header";
        case DolbySequentialPresentationReason::TruncatedFinalFrame: return "truncated_final_frame";
        case DolbySequentialPresentationReason::UnsupportedBitstreamId: return "unsupported_bitstream_id";
        case DolbySequentialPresentationReason::UnsupportedReducedRateEac3: return "unsupported_reduced_rate_eac3";
        case DolbySequentialPresentationReason::UnsupportedEac3StreamType: return "unsupported_eac3_stream_type";
        case DolbySequentialPresentationReason::UnsupportedEac3Substream: return "unsupported_eac3_substream";
        case DolbySequentialPresentationReason::MixedCodecFamily: return "mixed_codec_family";
        case DolbySequentialPresentationReason::SampleRateConflict: return "sample_rate_conflict";
        case DolbySequentialPresentationReason::ChannelConfigurationConflict: return "channel_configuration_conflict";
        case DolbySequentialPresentationReason::SampleRateChanged: return "sample_rate_changed";
        case DolbySequentialPresentationReason::ChannelConfigurationChanged: return "channel_configuration_changed";
        case DolbySequentialPresentationReason::BitstreamIdChanged: return "bitstream_id_changed";
        case DolbySequentialPresentationReason::UnexpectedTrailingData: return "unexpected_trailing_data";
        case DolbySequentialPresentationReason::CounterOverflow: return "counter_overflow";
        case DolbySequentialPresentationReason::PresentationOverflow: return "presentation_overflow";
        case DolbySequentialPresentationReason::FileMutatedDuringScan: return "file_mutated_during_scan";
        case DolbySequentialPresentationReason::ReadError: return "read_error";
        case DolbySequentialPresentationReason::ExactAuthorityConflict: return "exact_authority_conflict";
        case DolbySequentialPresentationReason::CompleteStrictAc3Inventory: return "complete_strict_ac3_inventory";
        case DolbySequentialPresentationReason::CompleteStrictEac3Inventory: return "complete_strict_eac3_inventory";
        case DolbySequentialPresentationReason::ProbeDocumentMissing: return "probe_document_missing";
        case DolbySequentialPresentationReason::ProbeDocumentInvalid: return "probe_document_invalid";
        case DolbySequentialPresentationReason::ProbeDocumentTooLarge: return "probe_document_too_large";
        case DolbySequentialPresentationReason::SourceIdentityMismatch: return "source_identity_mismatch";
        case DolbySequentialPresentationReason::AcceptedFastProbeEvidence: return "accepted_fast_probe_evidence";
    }
    return "not_eligible";
}

const char* dolbySequentialHandoffStatusName(
    DolbySequentialHandoffStatus status) noexcept {
    switch (status) {
        case DolbySequentialHandoffStatus::Accepted: return "accepted";
        case DolbySequentialHandoffStatus::Unavailable: return "unavailable";
        case DolbySequentialHandoffStatus::Conflict: return "conflict";
    }
    return "unavailable";
}

}  // namespace AveMediaBridge::Probe
