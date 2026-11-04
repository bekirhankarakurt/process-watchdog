/// @file logger.hpp
/// @brief Minimal leveled logger for the supervisor's own diagnostics (stderr).
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace pw {

/// Severity of a diagnostic message.
enum class LogLevel { Debug, Info, Warning, Error };

/// Parses `debug`, `info`, `warning`/`warn` or `error`.
[[nodiscard]] std::optional<LogLevel> parse_log_level(std::string_view text);
/// Returns the lower-case name of a level.
[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;

/// Sets the minimum level that is written; the default is Info.
void set_log_level(LogLevel level) noexcept;
/// Returns the current minimum level.
[[nodiscard]] LogLevel log_level() noexcept;

/// Writes `<timestamp> [level] message` to stderr if @p level is enabled.
void log(LogLevel level, std::string_view message);

/// Local wall-clock time formatted as `YYYY-MM-DDTHH:MM:SS.mmm`.
[[nodiscard]] std::string timestamp_now();

}  // namespace pw
