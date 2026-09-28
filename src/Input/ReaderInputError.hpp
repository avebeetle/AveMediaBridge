#pragma once

#include <stdexcept>
#include <string>

namespace AveMediaBridge::Input {
enum class ReaderInputFailure { Canceled, InputFailed, Unsupported, BindingMismatch };

class ReaderInputError final : public std::runtime_error {
public:
    ReaderInputError(ReaderInputFailure failure, std::string message)
        : std::runtime_error(std::move(message)), failure_(failure) {}
    ReaderInputFailure failure() const noexcept { return failure_; }

private:
    ReaderInputFailure failure_;
};
}
