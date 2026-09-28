#pragma once
#include <filesystem>
#include <stdexcept>
#include <string>
#include <windows.h>

inline std::filesystem::path acquireOwnedExportTestRoot(
    const std::string& prefix, const std::filesystem::path& firstCandidate = {}) {
    const auto parent = std::filesystem::temp_directory_path();
    const auto nonce = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        const auto path = attempt == 0 && !firstCandidate.empty()
            ? firstCandidate : parent / (prefix + nonce + "-" + std::to_string(attempt));
        std::error_code error;
        if (std::filesystem::create_directory(path, error)) return path;
        if (error) throw std::filesystem::filesystem_error("acquire export test root", path, error);
    }
    throw std::runtime_error("could not acquire unique export test root");
}
