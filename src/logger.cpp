#include "pw/logger.hpp"

#include "pw/util.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace pw {
namespace {

std::atomic<LogLevel>& level_storage() noexcept
{
    static std::atomic<LogLevel> level{LogLevel::Info};
    return level;
}

}  // namespace

std::optional<LogLevel> parse_log_level(std::string_view text)
{
    const std::string lowered = to_lower(trim(text));
    if (lowered == "debug") {
        return LogLevel::Debug;
    }
    if (lowered == "info") {
        return LogLevel::Info;
    }
    if (lowered == "warning" || lowered == "warn") {
        return LogLevel::Warning;
    }
    if (lowered == "error") {
        return LogLevel::Error;
    }
    return std::nullopt;
}

std::string_view to_string(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warning:
        return "warning";
    case LogLevel::Error:
        return "error";
    }
    return "unknown";
}

void set_log_level(LogLevel level) noexcept
{
    level_storage().store(level);
}

LogLevel log_level() noexcept
{
    return level_storage().load();
}

void log(LogLevel level, std::string_view message)
{
    if (static_cast<int>(level) < static_cast<int>(log_level())) {
        return;
    }
    const std::string line = timestamp_now() + " [" + std::string(to_string(level)) + "] " + std::string(message) + "\n";
    static_cast<void>(std::fwrite(line.data(), 1, line.size(), stderr));
    static_cast<void>(std::fflush(stderr));
}

std::string timestamp_now()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm local{};
    static_cast<void>(localtime_r(&seconds, &local));
    std::array<char, 32> date{};
    const std::size_t length = std::strftime(date.data(), date.size(), "%Y-%m-%dT%H:%M:%S", &local);
    std::array<char, 8> fraction{};
    static_cast<void>(std::snprintf(fraction.data(), fraction.size(), ".%03d", static_cast<int>(millis)));
    return std::string(date.data(), length) + fraction.data();
}

}  // namespace pw
