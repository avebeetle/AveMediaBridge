#define WIN32_LEAN_AND_MEAN
#include "ImportPcmWriter.hpp"
#include "AveMediaBridge/AveMediaBridgeApi.hpp"
#include <sddl.h>
#include <utility>

namespace AveMediaBridge::PcmOutput {
namespace {
struct Handle {
    HANDLE value;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

bool supportsGuard(HANDLE h) {
    wchar_t name[32]{};
    DWORD flags = 0;
    // Volume GUID paths are available for local volumes, not network shares.
    return GetFileType(h) == FILE_TYPE_DISK &&
        GetFinalPathNameByHandleW(h, nullptr, 0, FILE_NAME_OPENED | VOLUME_NAME_GUID) != 0 &&
        GetVolumeInformationByHandleW(h, nullptr, 0, nullptr, nullptr, &flags, name, 32) &&
        (flags & FILE_PERSISTENT_ACLS) && _wcsicmp(name, L"NTFS") == 0;
}

bool eligibleDirectory(const std::filesystem::path& path) {
    Handle dir{CreateFileW(path.parent_path().c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    return dir.value != INVALID_HANDLE_VALUE && supportsGuard(dir.value);
}
}

thread_local ImportScope* ImportScope::current = nullptr;
ImportScope::ImportScope(bool requestGuard) : requested(requestGuard), previous_(current) { current = this; }
ImportScope::~ImportScope() { current = previous_; }

void removeOwnedFile(const std::filesystem::path& path) {
    const auto* scope = ImportScope::current;
    if (!scope || !scope->identified) return;
    Handle h{CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    BY_HANDLE_FILE_INFORMATION id{};
    if (h.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h.value,&id)) return;
    const auto& expected = scope->fileId;
    if (id.dwVolumeSerialNumber != expected.dwVolumeSerialNumber ||
        id.nFileIndexHigh != expected.nFileIndexHigh || id.nFileIndexLow != expected.nFileIndexLow) return;
    FILE_DISPOSITION_INFO disposition{TRUE};
    SetFileInformationByHandle(h.value, FileDispositionInfo, &disposition, sizeof(disposition));
}

Writer::~Writer() { if (is_open()) close(); }
void Writer::open(const std::filesystem::path& path) {
    auto& scope = *ImportScope::current;
    native_ = scope.requested;
    if (!native_) {
        legacy_.open(path, std::ios::binary | std::ios::trunc);
        return;
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    // Deny access to modify the data, DACL and owner on subsequent opens.
    // OWNER RIGHTS suppresses the owner's implicit permission to rewrite the DACL.
    const bool protectedCreate = eligibleDirectory(path) && ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(D;;0x000c0006;;;WD)(D;;0x000c0006;;;OW)(A;;GRSD;;;OW)(A;;GRSD;;;SY)",
        SDDL_REVISION_1, &descriptor, nullptr);
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    file_ = CreateFileW(path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        protectedCreate ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE,
        protectedCreate ? &security : nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (descriptor) LocalFree(descriptor);
    if (!is_open()) return;
    scope.identified = GetFileInformationByHandle(file_, &scope.fileId) != 0;
    // A filesystem unable to attest the exact protected identity still decodes.
    // Its caller must read the completed file to compute the fingerprint.
    scope.guarded = protectedCreate && scope.identified && supportsGuard(file_) &&
        (scope.fileId.nFileIndexHigh || scope.fileId.nFileIndexLow) &&
        !(scope.fileId.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
    good_ = true;
}
bool Writer::is_open() const { return native_ ? file_ != INVALID_HANDLE_VALUE : legacy_.is_open(); }
Writer::operator bool() const { return native_ ? good_ : static_cast<bool>(legacy_); }
void Writer::write(const char* data, std::streamsize size) {
    if (!native_) { legacy_.write(data,size); return; }
    if (!good_ || size < 0 || static_cast<unsigned long long>(size) > MAXDWORD) { good_ = false; return; }
    DWORD written = 0;
    good_ = WriteFile(file_, data, static_cast<DWORD>(size), &written, nullptr) && written == size;
}
void Writer::flush() {
    // Synchronous WriteFile has already published the bytes to the filesystem cache.
    if (!native_) legacy_.flush();
}
void Writer::close() {
    if (!native_) { legacy_.close(); return; }
    ImportScope::current->guarded = false;
    if (!CloseHandle(std::exchange(file_, INVALID_HANDLE_VALUE))) good_ = false;
}
} // namespace AveMediaBridge::PcmOutput

int __cdecl AveMediaBridge_GuardedPcmFileIdV1(unsigned long* volume, unsigned long* high, unsigned long* low) {
    const auto* scope = AveMediaBridge::PcmOutput::ImportScope::current;
    if (!volume || !high || !low || !scope || !scope->guarded) return 0;
    *volume = scope->fileId.dwVolumeSerialNumber;
    *high = scope->fileId.nFileIndexHigh;
    *low = scope->fileId.nFileIndexLow;
    return 1;
}
