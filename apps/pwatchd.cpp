// pwatchd - process supervisor daemon.

#include "pw/config.hpp"
#include "pw/logger.hpp"
#include "pw/supervisor.hpp"
#include "pw/version.hpp"

#include <cstdio>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view usage_text = R"(Usage: pwatchd -c FILE [options]

Supervise the services listed in an INI configuration file.

Options:
  -c, --config FILE      configuration file (required)
  -n, --check            validate the configuration, print a summary and exit
  -l, --log-level LEVEL  override log level: debug, info, warning, error
  -V, --version          print version and exit
  -h, --help             show this help
)";

struct Options {
    std::string config;
    bool check{false};
    std::optional<pw::LogLevel> level;
};

[[nodiscard]] std::string limits_summary(const pw::ServiceConfig& service)
{
    std::string text;
    for (const pw::ResourceLimit& limit : service.limits) {
        if (!text.empty()) {
            text += ",";
        }
        switch (limit.kind) {
        case pw::LimitKind::CpuSeconds:
            text += "cpu=" + std::to_string(limit.value) + "s";
            break;
        case pw::LimitKind::OpenFiles:
            text += "nofile=" + std::to_string(limit.value);
            break;
        case pw::LimitKind::AddressSpace:
            text += "as=" + std::to_string(limit.value);
            break;
        case pw::LimitKind::CoreSize:
            text += "core=" + std::to_string(limit.value);
            break;
        case pw::LimitKind::Processes:
            text += "nproc=" + std::to_string(limit.value);
            break;
        case pw::LimitKind::FileSize:
            text += "fsize=" + std::to_string(limit.value);
            break;
        }
    }
    return text.empty() ? "-" : text;
}

void print_summary(const pw::WatchdogConfig& config)
{
    std::cout << "configuration OK: " << config.services.size() << " service(s)\n"
              << "  control socket   " << config.control_socket.string() << "\n"
              << "  heartbeat socket " << (config.heartbeat_socket ? config.heartbeat_socket->string() : "-") << "\n"
              << "  heartbeat udp    "
              << (config.heartbeat_udp
                      ? config.heartbeat_udp->host + ":" + std::to_string(config.heartbeat_udp->port)
                      : std::string("-"))
              << "\n"
              << "  log dir          " << config.log_dir.string() << "\n\n";
    std::printf("%-16s %-11s %-9s %-10s %-18s %s\n", "SERVICE", "POLICY", "RESTARTS", "HEARTBEAT", "LIMITS",
                "COMMAND");
    for (const pw::ServiceConfig& service : config.services) {
        std::string command;
        for (const std::string& word : service.argv) {
            command += (command.empty() ? "" : " ") + word;
        }
        const std::string restarts =
            service.restart.max_restarts ? std::to_string(*service.restart.max_restarts) : "unlimited";
        const std::string heartbeat =
            service.heartbeat.enabled() ? pw::format_duration(service.heartbeat.timeout) : "off";
        std::printf("%-16s %-11s %-9s %-10s %-18s %s%s\n", service.name.c_str(),
                    std::string(pw::to_string(service.restart.policy)).c_str(), restarts.c_str(), heartbeat.c_str(),
                    limits_summary(service).c_str(), command.c_str(), service.autostart ? "" : "  (manual start)");
    }
}

/// Returns the parsed options, or an exit code when the program should stop.
[[nodiscard]] std::optional<int> parse_arguments(const std::vector<std::string_view>& args, Options& options)
{
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        const auto next = [&]() -> std::optional<std::string_view> {
            if (i + 1 >= args.size()) {
                std::cerr << "pwatchd: " << arg << " needs a value\n";
                return std::nullopt;
            }
            return args[++i];
        };
        if (arg == "-h" || arg == "--help") {
            std::cout << usage_text;
            return 0;
        }
        if (arg == "-V" || arg == "--version") {
            std::cout << "pwatchd " << pw::version << "\n";
            return 0;
        }
        if (arg == "-n" || arg == "--check") {
            options.check = true;
        } else if (arg == "-c" || arg == "--config") {
            const auto value = next();
            if (!value) {
                return 2;
            }
            options.config = std::string(*value);
        } else if (arg == "-l" || arg == "--log-level") {
            const auto value = next();
            if (!value) {
                return 2;
            }
            options.level = pw::parse_log_level(*value);
            if (!options.level) {
                std::cerr << "pwatchd: invalid log level '" << *value << "'\n";
                return 2;
            }
        } else {
            std::cerr << "pwatchd: unknown argument '" << arg << "'\n" << usage_text;
            return 2;
        }
    }
    if (options.config.empty()) {
        std::cerr << "pwatchd: missing -c FILE\n" << usage_text;
        return 2;
    }
    return std::nullopt;
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        const std::vector<std::string_view> args(argv + 1, argv + argc);
        Options options;
        if (const auto code = parse_arguments(args, options)) {
            return *code;
        }
        pw::WatchdogConfig config = pw::load_config(options.config);
        if (options.level) {
            config.log_level = *options.level;
        }
        if (options.check) {
            print_summary(config);
            return 0;
        }
        pw::log(pw::LogLevel::Info, "pwatchd " + std::string(pw::version) + " supervising " +
                                        std::to_string(config.services.size()) + " service(s)");
        pw::Supervisor supervisor(std::move(config));
        return supervisor.run();
    } catch (const pw::ConfigError& error) {
        std::cerr << "pwatchd: config error: " << error.what() << "\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "pwatchd: " << error.what() << "\n";
        return 1;
    }
}
