#pragma once
#include "../Input/MediaInputSource.hpp"
#include "../Input/ReaderInputError.hpp"
#include "../Probe/MediaProbeService.hpp"
#include "AveMediaBridge/AveMediaBridgeReaderApi.h"
#include <utility>

struct AMBR_PreparedInput {
    AveMediaBridge::Input::MediaInputSource source;
    AveMediaBridge::Probe::FastProbeResult probe;
    std::string mediaFactsJson;
    bool attempted = false;
};

namespace AveMediaBridge::Input {
template <class F> class ScopeExit final {
  public:
    explicit ScopeExit(F f) : f_(std::move(f)) {}
    ~ScopeExit() noexcept { f_(); }
    ScopeExit(const ScopeExit &) = delete;
    ScopeExit &operator=(const ScopeExit &) = delete;

  private:
    F f_;
};

// The wrapper lives for one synchronous import. It forwards every read to
// the copied descriptor and joins source and import cancellation for all opens.
struct ImportSourceCallbacks {
    AMBI_SourceV1 original;
    AveMediaBridgeCancelCallback cancel;
    void *user;
    static AMBI_Status __cdecl read(void *self, uint64_t offset, void *dst, uint32_t requested,
                                    uint32_t *count) {
        const auto &s = *static_cast<ImportSourceCallbacks *>(self);
        return s.original.readAt(s.original.user, offset, dst, requested, count);
    }
    static AMBI_Status __cdecl check(void *self) {
        const auto &s = *static_cast<ImportSourceCallbacks *>(self);
        const auto status = s.original.checkCancel(s.original.user);
        if (status != AMBI_OK)
            return status;
        return s.cancel && s.cancel(s.user) ? AMBI_CANCELED : AMBI_OK;
    }
    MediaInputSource source(const std::string &label) {
        auto descriptor = original;
        descriptor.user = this;
        descriptor.readAt = &read;
        descriptor.checkCancel = &check;
        return MediaInputSource::fromStable(descriptor, label);
    }
};
} // namespace AveMediaBridge::Input
