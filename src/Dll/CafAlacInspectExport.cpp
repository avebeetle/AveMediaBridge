#include "AveMediaBridge/AveMediaBridgeSourceInspectApi.h"
#include "Probe/QueuedSourceSha256.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
using Hash = AveMediaBridge::Probe::QueuedSourceSha256;
using Byte = std::uint8_t;
using Count = std::uint64_t;
struct Unsupported {};
struct ReadError {};
struct Canceled {};

constexpr Count maxPacketTableBytes = 4 * 1024 * 1024;
constexpr Count maxPacketCount = 1000000;
constexpr unsigned maxChunks = 256;

Count readBigEndian(const Byte *p, unsigned n) {
    Count result = 0;
    while (n--)
        result = (result << 8) | *p++;
    return result;
}
void requireEvidence(bool ok) {
    if (!ok)
        throw Unsupported{};
}
void checkCancel(Hash &hash) {
    if (hash.canceled())
        throw Canceled{};
}

struct PinnedReader {
    HANDLE file;
    explicit PinnedReader(HANDLE pin)
        : file(ReOpenFile(pin, GENERIC_READ, FILE_SHARE_READ, FILE_FLAG_SEQUENTIAL_SCAN)) {
        if (file == INVALID_HANDLE_VALUE)
            throw ReadError{};
    }
    ~PinnedReader() { CloseHandle(file); }
    PinnedReader(const PinnedReader &) = delete;
    PinnedReader &operator=(const PinnedReader &) = delete;
    void seek(Count offset) {
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(file, pos, nullptr, FILE_BEGIN))
            throw ReadError{};
    }
    void read(Byte *out, DWORD count) {
        DWORD got = 0;
        if (!ReadFile(file, out, count, &got, nullptr) || got != count)
            throw ReadError{};
    }
    void readAt(Count offset, Byte *out, DWORD count) {
        seek(offset);
        read(out, count);
    }
};

struct ChunkRange {
    Count pos = 0, bytes = 0;
};
struct PacketPlan {
    Count dataStart = 0, declared = 0, packets = 0;
    unsigned channels = 0, rate = 0, maxFrames = 0;
    bool variable = false;
    std::vector<Byte> table;

    unsigned readPacketField(std::size_t &index) const {
        Count number = 0;
        for (unsigned i = 0; i < 5 && index < table.size(); ++i) {
            const Byte b = table[index++];
            number = (number << 7) | (b & 127);
            if (!(b & 128)) {
                requireEvidence(number && number <= UINT32_MAX);
                return static_cast<unsigned>(number);
            }
        }
        throw Unsupported{};
    }
};

PacketPlan readPacketPlan(PinnedReader &io, Count fileBytes, Hash &hash) {
    Byte h[48]{};
    requireEvidence(fileBytes >= 8);
    io.readAt(0, h, 8);
    requireEvidence(readBigEndian(h, 8) == UINT64_C(0x6361666600010000));
    ChunkRange desc, cookie, data, packetTable;
    unsigned chunkCount = 0;
    for (Count at = 8; at < fileBytes;) {
        checkCancel(hash);
        requireEvidence(++chunkCount <= maxChunks && fileBytes - at >= 12);
        io.readAt(at, h, 12);
        if (chunkCount == 1) {
            requireEvidence(std::memcmp(h, "desc", 4) == 0);
        }
        const Count size = readBigEndian(h + 4, 8);
        at += 12;
        requireEvidence(size <= fileBytes - at);
        ChunkRange *chunk = nullptr;
        if (!std::memcmp(h, "desc", 4))
            chunk = &desc;
        if (!std::memcmp(h, "kuki", 4))
            chunk = &cookie;
        if (!std::memcmp(h, "data", 4))
            chunk = &data;
        if (!std::memcmp(h, "pakt", 4))
            chunk = &packetTable;
        if (chunk) {
            requireEvidence(!chunk->pos);
            *chunk = {at, size};
        }
        at += size;
    }
    requireEvidence(desc.bytes == 32 && cookie.bytes == 48 && packetTable.bytes > 24 &&
                    packetTable.bytes <= maxPacketTableBytes && data.bytes > 4);
    PacketPlan plan;
    io.readAt(desc.pos, h, 32);
    const auto rateBits = readBigEndian(h, 8);
    double rate;
    std::memcpy(&rate, &rateBits, sizeof(rate));
    requireEvidence(std::isfinite(rate) && rate >= 1 && rate <= 384000 && rate == std::floor(rate));
    plan.rate = static_cast<unsigned>(rate);
    requireEvidence(!std::memcmp(h + 8, "alac", 4) && readBigEndian(h + 12, 4) == 0 &&
                    readBigEndian(h + 16, 4) == 0);
    const auto fixedFrames = readBigEndian(h + 20, 4);
    plan.channels = static_cast<unsigned>(readBigEndian(h + 24, 4));
    requireEvidence(plan.channels == 1 || plan.channels == 2);
    io.readAt(cookie.pos, h, 48);
    requireEvidence(readBigEndian(h, 4) == 12 && !std::memcmp(h + 4, "frmaalac", 8) &&
                    readBigEndian(h + 12, 4) == 36 && !std::memcmp(h + 16, "alac", 4) &&
                    readBigEndian(h + 20, 4) == 0 && h[28] == 0 &&
                    (h[29] == 16 || h[29] == 20 || h[29] == 24 || h[29] == 32) && h[33] == plan.channels &&
                    readBigEndian(h + 44, 4) == plan.rate);
    plan.maxFrames = static_cast<unsigned>(readBigEndian(h + 24, 4));
    requireEvidence(plan.maxFrames && plan.maxFrames <= 65536 &&
                    (!fixedFrames || fixedFrames == plan.maxFrames));
    plan.variable = !fixedFrames;
    io.readAt(data.pos, h, 4);
    requireEvidence(!readBigEndian(h, 4)); // Active edits requireEvidence different presentation accounting.
    plan.dataStart = data.pos + 4;
    io.readAt(packetTable.pos, h, 24);
    plan.packets = readBigEndian(h, 8);
    plan.declared = readBigEndian(h + 8, 8);
    requireEvidence(plan.packets && plan.packets <= maxPacketCount && !readBigEndian(h + 16, 8) &&
                    plan.declared && plan.declared <= plan.packets * plan.maxFrames);
    plan.table.resize(static_cast<std::size_t>(packetTable.bytes - 24));
    io.read(plan.table.data(), static_cast<DWORD>(plan.table.size()));
    Count packetBytes = 0, variableFrames = 0;
    std::size_t index = 0;
    for (Count i = 0; i < plan.packets; ++i) {
        if (!(i % 4096))
            checkCancel(hash);
        const auto size = plan.readPacketField(index);
        requireEvidence(size >= 4 && size <= data.bytes - 4 - packetBytes);
        packetBytes += size;
        if (plan.variable) {
            const auto frames = plan.readPacketField(index);
            requireEvidence(frames <= plan.maxFrames);
            variableFrames += frames;
        }
    }
    requireEvidence(index == plan.table.size() && packetBytes == data.bytes - 4 &&
                    (!plan.variable || variableFrames == plan.declared));
    return plan;
}

class PacketHeaders {
  public:
    explicit PacketHeaders(const PacketPlan &plan) : plan_(plan), packetOffset_(plan.dataStart) {
        nextPacket();
    }
    void observe(const Byte *block, Count offset, DWORD size) {
        const Count limit = offset + size;
        while (packetsScanned_ < plan_.packets && packetOffset_ + headerBytesRead_ < limit) {
            const Count start = packetOffset_ + headerBytesRead_;
            requireEvidence(start >= offset);
            const auto take =
                static_cast<unsigned>(std::min<Count>(headerBytesNeeded_ - headerBytesRead_, limit - start));
            std::memcpy(header_.data() + headerBytesRead_, block + (start - offset), take);
            headerBytesRead_ += take;
            if (headerBytesRead_ != headerBytesNeeded_)
                break;
            const auto prefix = static_cast<unsigned>(readBigEndian(header_.data(), 3));
            // The leading three bits identify a mono single-channel element (0)
            // or a stereo channel-pair element (1). The following instance tag
            // and reserved bits must be zero for this qualified layout.
            requireEvidence((prefix >> 21) == (plan_.channels == 1 ? 0U : 1U) && !(prefix & 0x1fffe0U));
            unsigned count = plan_.maxFrames;
            // Bit 4 signals a partial packet. Its 32-bit sample count
            // occupies bits 23–54 of the packet header (counting from bit 0).
            if (prefix & 16) {
                requireEvidence(headerBytesNeeded_ == 7);
                count = static_cast<unsigned>((readBigEndian(header_.data() + 2, 5) >> 1) & UINT32_MAX);
            }
            requireEvidence(count && count <= plan_.maxFrames && (!plan_.variable || count == tableFrames_));
            frames_ += count;
            ++packetsScanned_;
            packetOffset_ += packetBytes_;
            if (packetsScanned_ < plan_.packets)
                nextPacket();
        }
    }
    // Some CAF writers store packet count times nominal capacity even
    // when interior or last packets contain fewer frames. The qualified
    // count sums the individual ALAC headers in that case.
    Count complete() const {
        if (packetsScanned_ != plan_.packets ||
            (frames_ != plan_.declared &&
             (plan_.variable || plan_.declared != plan_.packets * plan_.maxFrames)))
            return 0;
        return frames_;
    }

  private:
    void nextPacket() {
        packetBytes_ = plan_.readPacketField(index_);
        if (plan_.variable)
            tableFrames_ = plan_.readPacketField(index_);
        headerBytesRead_ = 0;
        headerBytesNeeded_ = std::min<unsigned>(7, packetBytes_);
    }
    const PacketPlan &plan_;
    Count packetOffset_, frames_ = 0, packetsScanned_ = 0;
    std::size_t index_ = 0;
    unsigned headerBytesRead_ = 0, headerBytesNeeded_ = 0, packetBytes_ = 0, tableFrames_ = 0;
    std::array<Byte, 7> header_{};
};
} // namespace

// A completed hash with unqualified late packet evidence returns 2. This
// preserves the ordinary presentation estimate without reading the source twice.
extern "C" AMBM_API int __cdecl AveMediaBridge_InspectCafAlacSourceV1(
    const wchar_t *path, void *pinned, AMBM_Cancel cancel, void *context, Byte *sha256, Count *bytes,
    Count *frames, std::int32_t *stream, std::int32_t *rate, std::int32_t *channels) {
    if (!path || !*path || !pinned || pinned == INVALID_HANDLE_VALUE || !sha256 || !bytes || !frames ||
        !stream || !rate || !channels)
        return -2;
    try {
        LARGE_INTEGER length{};
        if (GetFileType(pinned) != FILE_TYPE_DISK || !GetFileSizeEx(pinned, &length) || length.QuadPart < 0)
            return -2;
        const Count size = length.QuadPart;
        Hash hash(cancel, context);
        checkCancel(hash);
        PinnedReader io(pinned);
        const auto plan = readPacketPlan(io, size, hash);
        PacketHeaders headers(plan);
        std::vector<Byte> block(1024U * 1024);
        bool qualified = true;
        io.seek(0);
        if (!hash.start())
            return -2;
        for (Count offset = 0; offset < size;) {
            checkCancel(hash);
            const auto count = static_cast<DWORD>(std::min<Count>(block.size(), size - offset));
            io.read(block.data(), count);
            if (qualified) {
                try {
                    headers.observe(block.data(), offset, count);
                } catch (const Unsupported &) {
                    qualified = false;
                }
            }
            if (!hash.append(block.data(), count))
                return hash.canceled() ? -1 : -2;
            offset += count;
        }
        std::array<Byte, 32> digest{};
        if (!hash.finish(digest, size))
            return hash.canceled() ? -1 : -2;
        checkCancel(hash);
        const auto count = qualified ? headers.complete() : 0;
        std::copy(digest.begin(), digest.end(), sha256);
        *bytes = size;
        if (!count)
            return 2;
        *frames = count;
        *stream = 0;
        *rate = plan.rate;
        *channels = plan.channels;
        return 1;
    } catch (const Unsupported &) {
        return 0;
    } catch (const Canceled &) {
        return -1;
    } catch (...) {
        return -2;
    }
}
