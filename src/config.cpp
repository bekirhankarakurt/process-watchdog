#include "pw/config.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace pw {
namespace {

struct SignalEntry {
    std::string_view name;
    int number;
};

constexpr std::array<SignalEntry, 20> signal_table{{
    {"HUP", SIGHUP},   {"INT", SIGINT},   {"QUIT", SIGQUIT}, {"ILL", SIGILL},   {"TRAP", SIGTRAP},
    {"ABRT", SIGABRT}, {"BUS", SIGBUS},   {"FPE", SIGFPE},   {"KILL", SIGKILL}, {"USR1", SIGUSR1},
    {"SEGV", SIGSEGV}, {"USR2", SIGUSR2}, {"PIPE", SIGPIPE}, {"ALRM", SIGALRM}, {"TERM", SIGTERM},
    {"CHLD", SIGCHLD}, {"CONT", SIGCONT}, {"STOP", SIGSTOP}, {"TSTP", SIGTSTP}, {"SYS", SIGSYS},
}};

constexpr std::string_view service_prefix = "service:";

[[nodiscard]] bool valid_service_name(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 64) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
               c == '-' || c == '.';
    });
}

[[nodiscard]] std::filesystem::path resolve(const std::filesystem::path& base, std::string_view value)
{
    std::filesystem::path path{std::string(value)};
    if (path.is_relative()) {
        path = base / path;
    }
    return path.lexically_normal();
}

[[nodiscard]] Millis require_duration(const IniEntry& entry)
{
    const auto value = parse_duration(entry.value);
    if (!value) {
        throw ConfigError(entry.line, entry.key + ": invalid duration '" + entry.value + "' (use e.g. 500ms, 5s, 2m)");
    }
    return *value;
}

[[nodiscard]] std::uint64_t require_size(const IniEntry& entry)
{
    const auto value = parse_size(entry.value);
    if (!value) {
        throw ConfigError(entry.line, entry.key + ": invalid size '" + entry.value + "' (use e.g. 4096, 64K, 10M)");
    }
    return *value;
}

[[nodiscard]] std::uint64_t require_uint(const IniEntry& entry)
{
    const auto value = parse_uint(entry.value);
    if (!value) {
        throw ConfigError(entry.line, entry.key + ": expected a non-negative integer, got '" + entry.value + "'");
    }
    return *value;
}

[[nodiscard]] unsigned require_unsigned(const IniEntry& entry)
{
    const std::uint64_t value = require_uint(entry);
    if (value > std::numeric_limits<unsigned>::max()) {
        throw ConfigError(entry.line, entry.key + ": value too large");
    }
    return static_cast<unsigned>(value);
}

[[nodiscard]] bool require_bool(const IniEntry& entry)
{
    const auto value = parse_bool(entry.value);
    if (!value) {
        throw ConfigError(entry.line, entry.key + ": expected true/false, got '" + entry.value + "'");
    }
    return *value;
}

void parse_watchdog_section(const IniSection& section, const std::filesystem::path& base, WatchdogConfig& config)
{
    for (const IniEntry& entry : section.entries) {
        if (entry.key == "control_socket") {
            config.control_socket = resolve(base, entry.value);
        } else if (entry.key == "heartbeat_socket") {
            config.heartbeat_socket = resolve(base, entry.value);
        } else if (entry.key == "heartbeat_udp") {
            config.heartbeat_udp = parse_endpoint(entry.value);
            if (!config.heartbeat_udp) {
                throw ConfigError(entry.line, "heartbeat_udp: expected IPv4 'host:port', got '" + entry.value + "'");
            }
        } else if (entry.key == "log_dir") {
            config.log_dir = resolve(base, entry.value);
        } else if (entry.key == "log_level") {
            const auto level = parse_log_level(entry.value);
            if (!level) {
                throw ConfigError(entry.line, "log_level: expected debug, info, warning or error");
            }
            config.log_level = *level;
        } else {
            throw ConfigError(entry.line, "unknown key '" + entry.key + "' in [watchdog]");
        }
    }
}

[[nodiscard]] bool parse_limit(const IniEntry& entry, ServiceConfig& service)
{
    static constexpr std::array<std::pair<std::string_view, LimitKind>, 6> limit_keys{{
        {"limit_cpu", LimitKind::CpuSeconds},
        {"limit_nofile", LimitKind::OpenFiles},
        {"limit_as", LimitKind::AddressSpace},
        {"limit_core", LimitKind::CoreSize},
        {"limit_nproc", LimitKind::Processes},
        {"limit_fsize", LimitKind::FileSize},
    }};
    const auto it = std::find_if(limit_keys.begin(), limit_keys.end(),
                                 [&](const auto& item) { return item.first == entry.key; });
    if (it == limit_keys.end()) {
        return false;
    }
    std::uint64_t value = 0;
    if (it->second == LimitKind::CpuSeconds) {
        value = static_cast<std::uint64_t>(require_duration(entry).count() / 1000);
        if (value == 0) {
            throw ConfigError(entry.line, "limit_cpu: must be at least 1s");
        }
    } else if (it->second == LimitKind::OpenFiles || it->second == LimitKind::Processes) {
        value = require_uint(entry);
    } else {
        value = require_size(entry);
    }
    service.limits.push_back(ResourceLimit{it->second, value});
    return true;
}

void parse_service_entry(const IniEntry& entry, const std::filesystem::path& base, ServiceConfig& service,
                         bool& grace_set)
{
    const std::string& key = entry.key;
    if (key == "command") {
        try {
            service.argv = split_command(entry.value);
        } catch (const std::invalid_argument& error) {
            throw ConfigError(entry.line, "command: " + std::string(error.what()));
        }
        if (service.argv.empty()) {
            throw ConfigError(entry.line, "command: must not be empty");
        }
    } else if (key == "working_dir") {
        service.working_dir = resolve(base, entry.value);
    } else if (key == "env") {
        const std::size_t equals = entry.value.find('=');
        if (equals == std::string::npos || equals == 0) {
            throw ConfigError(entry.line, "env: expected KEY=VALUE");
        }
        service.env.push_back(entry.value);
    } else if (key == "restart") {
        const auto policy = parse_restart_policy(entry.value);
        if (!policy) {
            throw ConfigError(entry.line, "restart: expected never, on-failure or always");
        }
        service.restart.policy = *policy;
    } else if (key == "backoff_initial") {
        service.restart.backoff.initial = require_duration(entry);
    } else if (key == "backoff_max") {
        service.restart.backoff.max = require_duration(entry);
    } else if (key == "backoff_multiplier") {
        const auto value = parse_double(entry.value);
        if (!value || *value < 1.0 || *value > 100.0) {
            throw ConfigError(entry.line, "backoff_multiplier: expected a number between 1 and 100");
        }
        service.restart.backoff.multiplier = *value;
    } else if (key == "max_restarts") {
        if (to_lower(entry.value) == "unlimited") {
            service.restart.max_restarts.reset();
        } else {
            service.restart.max_restarts = require_unsigned(entry);
        }
    } else if (key == "stable_after") {
        service.restart.stable_after = require_duration(entry);
    } else if (key == "heartbeat_timeout") {
        service.heartbeat.timeout = require_duration(entry);
    } else if (key == "heartbeat_grace") {
        service.heartbeat.grace = require_duration(entry);
        grace_set = true;
    } else if (key == "stop_signal") {
        const auto signal_number = parse_signal(entry.value);
        if (!signal_number) {
            throw ConfigError(entry.line, "stop_signal: unknown signal '" + entry.value + "'");
        }
        service.stop_signal = *signal_number;
    } else if (key == "stop_timeout") {
        service.stop_timeout = require_duration(entry);
    } else if (key == "autostart") {
        service.autostart = require_bool(entry);
    } else if (key == "log_file") {
        service.log.file = resolve(base, entry.value);
    } else if (key == "log_max_size") {
        service.log.max_bytes = require_size(entry);
    } else if (key == "log_max_files") {
        service.log.max_files = require_unsigned(entry);
    } else if (key == "log_timestamps") {
        service.log.timestamps = require_bool(entry);
    } else if (!parse_limit(entry, service)) {
        throw ConfigError(entry.line, "unknown key '" + key + "' in [service:" + service.name + "]");
    }
}

[[nodiscard]] ServiceConfig parse_service_section(const IniSection& section, const std::filesystem::path& base,
                                                  const WatchdogConfig& config)
{
    ServiceConfig service;
    service.name = section.name.substr(service_prefix.size());
    if (!valid_service_name(service.name)) {
        throw ConfigError(section.line, "invalid service name '" + service.name + "' (use [A-Za-z0-9_.-], max 64)");
    }
    bool grace_set = false;
    for (const IniEntry& entry : section.entries) {
        parse_service_entry(entry, base, service, grace_set);
    }
    if (service.argv.empty()) {
        throw ConfigError(section.line, "service '" + service.name + "' has no command");
    }
    if (service.restart.backoff.max < service.restart.backoff.initial) {
        throw ConfigError(section.line, "service '" + service.name + "': backoff_max is smaller than backoff_initial");
    }
    if (!grace_set) {
        service.heartbeat.grace = service.heartbeat.timeout;
    }
    if (service.heartbeat.enabled() && !config.heartbeat_socket && !config.heartbeat_udp) {
        throw ConfigError(section.line, "service '" + service.name +
                                            "' uses heartbeat_timeout but [watchdog] defines no heartbeat_socket "
                                            "or heartbeat_udp");
    }
    if (service.log.file.empty()) {
        service.log.file = config.log_dir / (service.name + ".log");
    }
    return service;
}

}  // namespace

WatchdogConfig parse_config(std::string_view text, const std::filesystem::path& base_dir)
{
    const std::vector<IniSection> sections = parse_ini(text);
    WatchdogConfig config;
    config.log_dir = resolve(base_dir, "logs");

    bool seen_watchdog = false;
    for (const IniSection& section : sections) {
        if (section.name == "watchdog") {
            if (seen_watchdog) {
                throw ConfigError(section.line, "duplicate [watchdog] section");
            }
            seen_watchdog = true;
            parse_watchdog_section(section, base_dir, config);
        } else if (section.name.rfind(service_prefix, 0) != 0) {
            throw ConfigError(section.line, "unknown section [" + section.name + "]");
        }
    }

    std::set<std::string> names;
    for (const IniSection& section : sections) {
        if (section.name == "watchdog") {
            continue;
        }
        ServiceConfig service = parse_service_section(section, base_dir, config);
        if (!names.insert(service.name).second) {
            throw ConfigError(section.line, "duplicate service '" + service.name + "'");
        }
        config.services.push_back(std::move(service));
    }
    if (config.services.empty()) {
        throw ConfigError(0, "no [service:NAME] sections defined");
    }
    return config;
}

WatchdogConfig load_config(const std::filesystem::path& file)
{
    std::ifstream stream(file);
    if (!stream) {
        throw ConfigError(0, "cannot read configuration file '" + file.string() + "'");
    }
    std::ostringstream content;
    content << stream.rdbuf();
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(file, error);
    const std::filesystem::path base = error ? file.parent_path() : absolute.parent_path();
    return parse_config(content.str(), base);
}

std::optional<UdpEndpoint> parse_endpoint(std::string_view text)
{
    text = trim(text);
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0) {
        return std::nullopt;
    }
    std::string host(text.substr(0, colon));
    if (host == "localhost") {
        host = "127.0.0.1";
    }
    in_addr address{};
    if (inet_pton(AF_INET, host.c_str(), &address) != 1) {
        return std::nullopt;
    }
    const auto port = parse_uint(text.substr(colon + 1));
    if (!port || *port == 0 || *port > std::numeric_limits<std::uint16_t>::max()) {
        return std::nullopt;
    }
    return UdpEndpoint{std::move(host), static_cast<std::uint16_t>(*port)};
}

std::optional<RestartPolicy> parse_restart_policy(std::string_view text)
{
    const std::string lowered = to_lower(trim(text));
    if (lowered == "never" || lowered == "no") {
        return RestartPolicy::Never;
    }
    if (lowered == "on-failure" || lowered == "on_failure") {
        return RestartPolicy::OnFailure;
    }
    if (lowered == "always") {
        return RestartPolicy::Always;
    }
    return std::nullopt;
}

std::string_view to_string(RestartPolicy policy) noexcept
{
    switch (policy) {
    case RestartPolicy::Never:
        return "never";
    case RestartPolicy::OnFailure:
        return "on-failure";
    case RestartPolicy::Always:
        return "always";
    }
    return "unknown";
}

std::optional<int> parse_signal(std::string_view text)
{
    text = trim(text);
    if (const auto number = parse_uint(text)) {
        if (*number >= 1 && *number <= 64) {
            return static_cast<int>(*number);
        }
        return std::nullopt;
    }
    std::string upper(text);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; });
    std::string_view name = upper;
    if (name.rfind("SIG", 0) == 0) {
        name.remove_prefix(3);
    }
    const auto it = std::find_if(signal_table.begin(), signal_table.end(),
                                 [&](const SignalEntry& entry) { return entry.name == name; });
    if (it == signal_table.end()) {
        return std::nullopt;
    }
    return it->number;
}

std::string signal_name(int signal_number)
{
    const auto it = std::find_if(signal_table.begin(), signal_table.end(),
                                 [&](const SignalEntry& entry) { return entry.number == signal_number; });
    if (it == signal_table.end()) {
        return "SIG" + std::to_string(signal_number);
    }
    return "SIG" + std::string(it->name);
}

}  // namespace pw
