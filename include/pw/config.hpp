/// @file config.hpp
/// @brief Watchdog and service configuration model and INI loader.
#pragma once

#include "pw/ini.hpp"
#include "pw/logger.hpp"
#include "pw/util.hpp"

#include <csignal>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pw {

/// When a service is restarted after its process ends on its own.
enum class RestartPolicy {
    Never,      ///< Never restart automatically.
    OnFailure,  ///< Restart on non-zero exit, signal or heartbeat timeout.
    Always,     ///< Restart regardless of how the process ended.
};

/// Resource limits applied with setrlimit() in the child before exec.
enum class LimitKind {
    CpuSeconds,    ///< RLIMIT_CPU (seconds)
    OpenFiles,     ///< RLIMIT_NOFILE
    AddressSpace,  ///< RLIMIT_AS (bytes)
    CoreSize,      ///< RLIMIT_CORE (bytes)
    Processes,     ///< RLIMIT_NPROC
    FileSize,      ///< RLIMIT_FSIZE (bytes)
};

/// One resource limit; soft and hard limits are both set to @ref value.
struct ResourceLimit {
    LimitKind kind{LimitKind::OpenFiles};
    std::uint64_t value{0};
};

/// Exponential backoff between automatic restarts.
struct BackoffSpec {
    Millis initial{500};
    Millis max{30000};
    double multiplier{2.0};
};

/// Restart behaviour of a service.
struct RestartSpec {
    RestartPolicy policy{RestartPolicy::OnFailure};
    BackoffSpec backoff{};
    /// Consecutive automatic restarts allowed before giving up; empty = unlimited.
    std::optional<unsigned> max_restarts{5U};
    /// A run lasting at least this long resets the restart counter and backoff.
    Millis stable_after{10000};
};

/// Heartbeat supervision; disabled when @ref timeout is zero.
struct HeartbeatSpec {
    Millis timeout{0};
    /// Time allowed after start before the first heartbeat is due.
    Millis grace{0};
    [[nodiscard]] bool enabled() const noexcept { return timeout.count() > 0; }
};

/// Where and how a service's stdout/stderr is stored.
struct LogSpec {
    std::filesystem::path file;
    /// Rotate when the file would exceed this size; 0 disables rotation.
    std::uint64_t max_bytes{10ULL * 1024ULL * 1024ULL};
    /// Number of rotated files (`.1` … `.N`) to keep; 0 truncates in place.
    unsigned max_files{5U};
    /// Prefix every captured line with a local timestamp.
    bool timestamps{true};
};

/// Full description of one supervised service.
struct ServiceConfig {
    std::string name;
    std::vector<std::string> argv;
    std::optional<std::filesystem::path> working_dir;
    /// Extra `KEY=VALUE` variables added to the inherited environment.
    std::vector<std::string> env;
    RestartSpec restart{};
    HeartbeatSpec heartbeat{};
    std::vector<ResourceLimit> limits;
    LogSpec log{};
    int stop_signal{SIGTERM};
    Millis stop_timeout{5000};
    bool autostart{true};
};

/// IPv4 UDP endpoint (`host:port`).
struct UdpEndpoint {
    std::string host;
    std::uint16_t port{0};
};

/// Top-level configuration: the `[watchdog]` section plus all services.
struct WatchdogConfig {
    std::filesystem::path control_socket{"/tmp/pwatchd.sock"};
    std::optional<std::filesystem::path> heartbeat_socket;
    std::optional<UdpEndpoint> heartbeat_udp;
    std::filesystem::path log_dir{"logs"};
    LogLevel log_level{LogLevel::Info};
    std::vector<ServiceConfig> services;
};

/// Builds a configuration from INI text. Relative paths are resolved against
/// @p base_dir (normally the directory of the configuration file).
/// @throws ConfigError on syntax errors, unknown keys or invalid values.
[[nodiscard]] WatchdogConfig parse_config(std::string_view text, const std::filesystem::path& base_dir);

/// Reads and parses a configuration file.
/// @throws ConfigError if the file cannot be read or is invalid.
[[nodiscard]] WatchdogConfig load_config(const std::filesystem::path& file);

/// Parses `host:port` (numeric IPv4 or `localhost`).
[[nodiscard]] std::optional<UdpEndpoint> parse_endpoint(std::string_view text);

/// Parses `never`, `on-failure` or `always`.
[[nodiscard]] std::optional<RestartPolicy> parse_restart_policy(std::string_view text);
[[nodiscard]] std::string_view to_string(RestartPolicy policy) noexcept;

/// Parses `TERM`, `SIGTERM` or `15`.
[[nodiscard]] std::optional<int> parse_signal(std::string_view text);
/// Returns `SIGTERM` style names, or `SIG<n>` for unknown numbers.
[[nodiscard]] std::string signal_name(int signal_number);

}  // namespace pw
