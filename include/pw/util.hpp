/// @file util.hpp
/// @brief Small parsing and formatting helpers shared by the library.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pw {

/// Millisecond duration used for every configurable time value.
using Millis = std::chrono::milliseconds;
/// Monotonic clock used for all scheduling decisions.
using Clock = std::chrono::steady_clock;
/// Point in time on the monotonic clock.
using TimePoint = Clock::time_point;

/// Removes leading and trailing ASCII whitespace.
[[nodiscard]] std::string_view trim(std::string_view text) noexcept;

/// Splits a command line into words without invoking a shell.
///
/// Supports single quotes (literal), double quotes (backslash escapes `"`, `\`
/// and `$`) and backslash escapes outside quotes.
/// @throws std::invalid_argument on an unterminated quote or trailing backslash.
[[nodiscard]] std::vector<std::string> split_command(std::string_view line);

/// Parses an unsigned decimal integer; rejects signs, spaces and overflow.
[[nodiscard]] std::optional<std::uint64_t> parse_uint(std::string_view text) noexcept;

/// Parses a finite, non-negative floating point number.
[[nodiscard]] std::optional<double> parse_double(std::string_view text) noexcept;

/// Parses a duration such as `250ms`, `5s`, `2m` or `1h`; a bare number means seconds.
[[nodiscard]] std::optional<Millis> parse_duration(std::string_view text) noexcept;

/// Parses a byte size such as `512`, `64K`, `10M` or `1G` (binary multiples).
[[nodiscard]] std::optional<std::uint64_t> parse_size(std::string_view text) noexcept;

/// Parses `true/false`, `yes/no`, `on/off` or `1/0` (case-insensitive).
[[nodiscard]] std::optional<bool> parse_bool(std::string_view text) noexcept;

/// Formats a duration compactly: `850ms`, `12.3s`, `4m05s`, `2h03m`, `3d04h`.
[[nodiscard]] std::string format_duration(Millis duration);

/// Returns the lower-cased copy of an ASCII string.
[[nodiscard]] std::string to_lower(std::string_view text);

}  // namespace pw
