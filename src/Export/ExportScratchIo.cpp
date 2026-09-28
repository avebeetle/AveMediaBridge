#include "Export/ExportScratchIo.hpp"
extern "C" {
#include <libavformat/avio.h>
#include <libavutil/mem.h>
#include <libavutil/error.h>
}
#include <stdexcept>

namespace AveMediaBridge::Export {
ExportScratchIo::ExportScratchIo(const std::filesystem::path& path, Fault fault) : fault_(fault) {
    if (!path.is_absolute()) throw std::runtime_error("scratch path must be absolute");
    wchar_t volume[MAX_PATH]{};
    if (!GetVolumePathNameW(path.c_str(), volume, MAX_PATH) || GetDriveTypeW(volume) != DRIVE_FIXED)
        throw std::runtime_error("scratch must be on a fixed local disk");
    for (auto parent = path.parent_path(); !parent.empty(); parent = parent.parent_path()) {
        const DWORD attributes = GetFileAttributesW(parent.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("scratch parent missing or redirected");
        if (parent == parent.root_path()) break;
    }
    file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) throw std::runtime_error("scratch open failed");
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    if (GetFileType(file_) != FILE_TYPE_DISK || !GetFileInformationByHandle(file_, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileSizeEx(file_, &size) || size.QuadPart != 0) {
        CloseHandle(file_); file_ = INVALID_HANDLE_VALUE;
        throw std::runtime_error("scratch must be an empty regular file");
    }
    auto* buffer = static_cast<unsigned char*>(av_malloc(32768));
    if (!buffer) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; throw std::bad_alloc(); }
    io_ = avio_alloc_context(buffer, 32768, 1, this, nullptr, &writePacket, &seek);
    if (!io_) { av_free(buffer); CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; throw std::bad_alloc(); }
    io_->seekable = AVIO_SEEKABLE_NORMAL;
}
ExportScratchIo::~ExportScratchIo() {
    abortClose();
}
void ExportScratchIo::abortClose() noexcept {
    if (io_) {
        av_freep(&io_->buffer);
        avio_context_free(&io_);
    }
    if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
}
int ExportScratchIo::writePacket(void* opaque, const uint8_t* data, int size) noexcept {
    auto* self = static_cast<ExportScratchIo*>(opaque);
    if (self->fault_ == Fault::Write) { self->error_ = AVERROR(EIO); return self->error_; }
    if (size < 0 || self->file_ == INVALID_HANDLE_VALUE) return AVERROR(EIO);
    DWORD written = 0;
    if (!WriteFile(self->file_, data, static_cast<DWORD>(size), &written, nullptr) || written != static_cast<DWORD>(size)) {
        self->error_ = AVERROR(EIO); return self->error_;
    }
    return size;
}
int64_t ExportScratchIo::seek(void* opaque, int64_t offset, int whence) noexcept {
    auto* self = static_cast<ExportScratchIo*>(opaque);
    if (self->fault_ == Fault::Seek) { self->error_ = AVERROR(EIO); return self->error_; }
    if (whence == AVSEEK_SIZE) {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(self->file_, &size)) { self->error_ = AVERROR(EIO); return self->error_; }
        return size.QuadPart;
    }
    LARGE_INTEGER move{}, result{};
    move.QuadPart = offset;
    DWORD method = whence == SEEK_SET ? FILE_BEGIN : whence == SEEK_CUR ? FILE_CURRENT : whence == SEEK_END ? FILE_END : ~DWORD(0);
    if (method == ~DWORD(0) || !SetFilePointerEx(self->file_, move, &result, method)) {
        self->error_ = AVERROR(EIO); return self->error_;
    }
    return result.QuadPart;
}
void ExportScratchIo::flushAndClose() {
    if (io_) avio_flush(io_);
    const bool bad = error_ || (io_ && io_->error) || fault_ == Fault::Flush || !FlushFileBuffers(file_);
    const bool closed = CloseHandle(file_) != 0;
    file_ = INVALID_HANDLE_VALUE;
    if (bad || !closed || fault_ == Fault::Close) throw std::runtime_error("scratch flush/close failed");
}
}
