/// @file fd.hpp
/// @brief RAII owner for POSIX file descriptors.
#pragma once

#include <unistd.h>

namespace pw {

/// Move-only owner of a file descriptor; closes it on destruction.
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    /// Returns the descriptor without giving up ownership.
    [[nodiscard]] int get() const noexcept { return fd_; }
    /// True when a descriptor is owned.
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    /// Gives up ownership and returns the descriptor.
    [[nodiscard]] int release() noexcept
    {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }
    /// Closes the owned descriptor (if any) and takes ownership of @p fd.
    void reset(int fd = -1) noexcept
    {
        if (fd_ >= 0) {
            static_cast<void>(::close(fd_));
        }
        fd_ = fd;
    }

private:
    int fd_{-1};
};

}  // namespace pw
