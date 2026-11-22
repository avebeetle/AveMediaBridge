#pragma once

#include "AveMediaBridge/AveMediaBridgeMatroskaInspectApi.h"
#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace AveMediaBridge::Probe {

class QueuedSourceSha256 {
public:
    QueuedSourceSha256(AMBM_Cancel cancel, void* context)
        : cancel_(cancel), context_(context) {}
    ~QueuedSourceSha256();
    bool start();
    bool canceled();
    bool append(const std::uint8_t* data, std::size_t size);
    bool finish(std::array<std::uint8_t, 32>& digest, std::uint64_t expectedBytes);

private:
    void consume();

    AMBM_Cancel cancel_;
    void* context_;
    bool canceled_ = false;  // Only queried by the caller thread.
    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_HASH_HANDLE hash_{};
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::array<std::vector<std::uint8_t>, 4> queue_;
    std::size_t head_ = 0, count_ = 0;
    std::uint64_t hashedBytes_ = 0;
    bool draining_ = false, abort_ = false, done_ = false, failed_ = false;
};

}  // namespace AveMediaBridge::Probe
