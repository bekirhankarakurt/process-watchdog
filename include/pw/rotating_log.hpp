/// @file rotating_log.hpp
/// @brief Size-based rotating log file for captured service output.
#pragma once

#include "pw/config.hpp"
#include "pw/fd.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace pw {

/// Appends data to `file`, rotating `file` → `file.1` → … → `file.N`.
class RotatingLog {
public:
    /// Opens (creating parent directories) the log described by @p spec.
    /// @throws std::system_error if the file cannot be opened.
    explicit RotatingLog(LogSpec spec);

    /// Appends @p data, inserting timestamps at line starts when enabled and
    /// rotating first if the write would exceed the size limit.
    /// @throws std::system_error on I/O failure.
    void write(std::string_view data);

    /// Bytes in the current (unrotated) file.
    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return spec_.file; }

    /// Path of rotated file number @p index (1-based).
    [[nodiscard]] std::filesystem::path rotated_path(unsigned index) const;

private:
    void open_file();
    void rotate();
    void write_all(std::string_view data);

    LogSpec spec_;
    UniqueFd fd_;
    std::uint64_t size_{0};
    bool at_line_start_{true};
};

}  // namespace pw
