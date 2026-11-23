#pragma once

#include "Decode/VorbisPresentation.hpp"
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

namespace AveMediaBridge::Vorbis::detail {
using Bytes = std::vector<std::uint8_t>;
inline void require(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}

class Source {
public:
    explicit Source(const Input::MediaInputSource& input);
    std::uint64_t size() const { return size_; }
    Status status() const { return status_; }
    bool readAt(std::uint64_t offset, void* destination, std::uint32_t bytes);
private:
    const AMBI_SourceV1* stable_;
    std::unique_ptr<FILE, decltype(&fclose)> file_{nullptr, fclose};
    std::uint64_t size_ = 0, nextOffset_ = 0;
    Status status_ = Status::Ok;
};

std::uint32_t little32(const std::uint8_t* data);
std::uint32_t pageChecksum(const Bytes& page);
Status readPackets(Source& source, void* consumer, Emit emit, void* cancelUser,
    Cancel cancel, std::uint64_t& sourceBytes, Plan* measured = nullptr);
}
