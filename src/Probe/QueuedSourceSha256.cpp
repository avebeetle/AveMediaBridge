#include "QueuedSourceSha256.hpp"

#include <chrono>

namespace AveMediaBridge::Probe {

bool QueuedSourceSha256::start() {
    if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
        return false;
    }
    worker_ = std::thread(&QueuedSourceSha256::consume, this);
    return true;
}

QueuedSourceSha256::~QueuedSourceSha256() {
    if (worker_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            abort_ = true;
        }
        changed_.notify_one();
        worker_.join();
    }
    if (hash_) BCryptDestroyHash(hash_);
    if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
}

bool QueuedSourceSha256::canceled() {
    canceled_ = canceled_ || (cancel_ && cancel_(context_) != 0);
    return canceled_;
}

bool QueuedSourceSha256::append(const std::uint8_t* data, std::size_t size) {
    if (canceled()) return false;
    std::unique_lock<std::mutex> lock(mutex_);
    while (count_ == queue_.size() && !failed_) {
        changed_.wait_for(lock, std::chrono::milliseconds(10));
        if (canceled()) return false;
    }
    if (failed_) return false;
    queue_[(head_ + count_) % queue_.size()].assign(data, data + size);
    ++count_;
    lock.unlock();
    changed_.notify_one();
    return true;
}

void QueuedSourceSha256::consume() {
    std::vector<std::uint8_t> block;
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        changed_.wait(lock, [this] { return count_ || draining_ || abort_; });
        if (abort_ || count_ == 0) break;
        block.swap(queue_[head_]);
        head_ = (head_ + 1) % queue_.size();
        --count_;
        lock.unlock();
        changed_.notify_one();
        const bool success = BCryptHashData(hash_, block.data(),
            static_cast<ULONG>(block.size()), 0) >= 0;
        lock.lock();
        if (!success) {
            failed_ = true;
            break;
        }
        hashedBytes_ += block.size();
    }
    done_ = true;
    lock.unlock();
    changed_.notify_one();
}

bool QueuedSourceSha256::finish(std::array<std::uint8_t, 32>& digest,
                                std::uint64_t expectedBytes) {
    std::unique_lock<std::mutex> lock(mutex_);
    draining_ = true;
    changed_.notify_one();
    while (!done_) {
        if (canceled()) return false;
        changed_.wait_for(lock, std::chrono::milliseconds(10));
    }
    if (failed_ || hashedBytes_ != expectedBytes) return false;
    lock.unlock();
    worker_.join();
    return !canceled() && BCryptFinishHash(hash_, digest.data(),
        static_cast<ULONG>(digest.size()), 0) >= 0;
}

}  // namespace AveMediaBridge::Probe
