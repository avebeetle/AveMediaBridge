#pragma once

#include <windows.h>
#include <filesystem>
#include <fstream>

namespace AveMediaBridge::PcmOutput {

// Stack discipline allows another import to run inside a progress callback.
struct ImportScope {
    explicit ImportScope(bool requestGuard);
    ~ImportScope();
    ImportScope(const ImportScope&) = delete;
    ImportScope& operator=(const ImportScope&) = delete;
    const bool requested;
    bool guarded = false;
    bool identified = false;
    BY_HANDLE_FILE_INFORMATION fileId{};
    static thread_local ImportScope* current;
private:
    ImportScope* previous_;
};

void removeOwnedFile(const std::filesystem::path& path);

class Writer final {
public:
    ~Writer();
    void open(const std::filesystem::path& path);
    bool is_open() const;
    explicit operator bool() const;
    void write(const char* data, std::streamsize size);
    void flush();
    void close();
private:
    // Original ABI clients retain their stream and overwrite semantics.
    std::ofstream legacy_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    bool native_ = false;
    bool good_ = false;
};
} // namespace AveMediaBridge::PcmOutput
