#include "Decode/VorbisInput.hpp"
#include "Input/StableInputContract.hpp"
#include <filesystem>

namespace AveMediaBridge::Vorbis::detail {
Source::Source(const Input::MediaInputSource &input) : stable_(input.stableSource()) {
    if (stable_) {
        if (Input::validateSource(stable_) == AMBI_OK)
            size_ = stable_->byteSize;
        else
            status_ = Status::IoError;
        return;
    }
    const auto path = std::filesystem::u8path(*input.legacyPath());
    file_.reset(_wfopen(path.c_str(), L"rb"));
    if (file_ && _fseeki64(file_.get(), 0, SEEK_END) == 0) {
        const auto length = _ftelli64(file_.get());
        if (length > 0 && _fseeki64(file_.get(), 0, SEEK_SET) == 0) {
            size_ = static_cast<std::uint64_t>(length);
            return;
        }
    }
    status_ = Status::IoError;
}

bool Source::readAt(std::uint64_t offset, void *destination, std::uint32_t bytes) {
    if (status_ != Status::Ok)
        return false;
    if (offset > size_ || bytes > size_ - offset)
        return false;
    if (stable_) {
        std::uint32_t received = 0;
        const auto result = Input::readSource(*stable_, offset, destination, bytes, received);
        if (result == AMBI_OK && received == bytes)
            return true;
        status_ = result == AMBI_CANCELED ? Status::Cancelled : Status::IoError;
        return false;
    }
    if ((offset == nextOffset_ || _fseeki64(file_.get(), static_cast<std::int64_t>(offset), SEEK_SET) == 0) &&
        std::fread(destination, 1, bytes, file_.get()) == bytes) {
        nextOffset_ = offset + bytes;
        return true;
    }
    status_ = Status::IoError;
    return false;
}
} // namespace AveMediaBridge::Vorbis::detail
