#pragma once
#include <filesystem>
#include <cstdint>
#include <windows.h>
struct AVIOContext;
namespace AveMediaBridge::Export {
class ExportScratchIo final {
public:
    enum class Fault { None, Write, Seek, Flush, Close };
    explicit ExportScratchIo(const std::filesystem::path& path, Fault fault = Fault::None);
    ~ExportScratchIo();
    ExportScratchIo(const ExportScratchIo&) = delete;
    ExportScratchIo& operator=(const ExportScratchIo&) = delete;
    AVIOContext* context() const noexcept { return io_; }
    void flushAndClose();
    bool failed() const noexcept { return error_ != 0; }
private:
    static int writePacket(void* opaque, const uint8_t* data, int size) noexcept;
    static int64_t seek(void* opaque, int64_t offset, int whence) noexcept;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    AVIOContext* io_ = nullptr;
    int error_ = 0;
    Fault fault_ = Fault::None;
};
}
