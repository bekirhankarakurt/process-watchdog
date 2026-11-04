// pwctl - control client for pwatchd.

#include "pw/config.hpp"
#include "pw/control.hpp"
#include "pw/heartbeat.hpp"
#include "pw/version.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

constexpr std::string_view usage_text = R"(Usage: pwctl [options] COMMAND [SERVICE|all]

Commands:
  list                    table of all services
  status SERVICE          detailed status of one service
  start SERVICE|all       start a stopped, exited or failed service
  stop SERVICE|all        stop a service (stop signal, then SIGKILL)
  restart SERVICE|all     stop and start again, resetting the restart counter
  shutdown                stop all services and terminate pwatchd
  beat [SERVICE]          send one heartbeat (inside a supervised service)

Options:
  -s, --socket PATH       control socket (default: $PW_CONTROL_SOCKET or /tmp/pwatchd.sock)
  -t, --timeout DURATION  request timeout (default: 5s)
      --raw               print raw protocol records
  -V, --version           print version and exit
  -h, --help              show this help

Exit status: 0 success, 1 request rejected, 2 usage error, 3 connection failure.
)";

constexpr int exit_rejected = 1;
constexpr int exit_usage = 2;
constexpr int exit_connection = 3;

struct Options {
    std::string socket;
    pw::Millis timeout{5000};
    bool raw{false};
    std::vector<std::string> words;
};

[[nodiscard]] std::string env_or(const char* name, std::string fallback)
{
    const char* const value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): single-threaded CLI
    return value != nullptr && *value != '\0' ? std::string(value) : fallback;
}

void print_table(const std::vector<std::string>& body)
{
    const std::vector<std::pair<std::string, std::string>> columns{
        {"name", "NAME"},         {"state", "STATE"},         {"pid", "PID"},
        {"restarts", "RESTARTS"}, {"uptime", "UPTIME"},       {"heartbeat", "HEARTBEAT"},
        {"last_exit", "LAST EXIT"},
    };
    std::vector<std::map<std::string, std::string>> rows;
    rows.reserve(body.size());
    for (const std::string& line : body) {
        rows.push_back(pw::parse_fields(line));
    }
    std::vector<std::size_t> widths;
    for (const auto& [key, title] : columns) {
        std::size_t width = title.size();
        for (auto& row : rows) {
            width = std::max(width, row[key].size());
        }
        widths.push_back(width);
    }
    const auto print_row = [&](const auto& cell) {
        std::string line;
        for (std::size_t i = 0; i < columns.size(); ++i) {
            std::string value = cell(i);
            if (i + 1 < columns.size()) {
                value.resize(widths[i] + 2, ' ');
            }
            line += value;
        }
        std::cout << line << "\n";
    };
    print_row([&](std::size_t i) { return columns[i].second; });
    for (auto& row : rows) {
        print_row([&](std::size_t i) { return row[columns[i].first]; });
    }
}

void print_status(const std::string& line)
{
    for (const auto& [key, value] : pw::parse_fields_ordered(line)) {
        std::printf("%-13s %s\n", (key + ":").c_str(), value.c_str());
    }
}

[[nodiscard]] int send_beat(const Options& options)
{
    const std::string service = options.words.size() > 1 ? options.words[1] : env_or("PW_SERVICE", "");
    const std::string token_hex = env_or("PW_HEARTBEAT_TOKEN", "");
    if (service.empty() || token_hex.empty()) {
        std::cerr << "pwctl: beat needs PW_SERVICE and PW_HEARTBEAT_TOKEN (set by pwatchd for its children)\n";
        return exit_usage;
    }
    const auto message = pw::parse_heartbeat("HB " + service + " " + token_hex);
    if (!message) {
        std::cerr << "pwctl: invalid PW_HEARTBEAT_TOKEN\n";
        return exit_usage;
    }
    const std::string datagram = pw::format_heartbeat(message->service, message->token);
    const std::string unix_socket = env_or("PW_HEARTBEAT_SOCKET", "");
    if (!unix_socket.empty()) {
        pw::send_heartbeat_unix(unix_socket, datagram);
        return 0;
    }
    const auto endpoint = pw::parse_endpoint(env_or("PW_HEARTBEAT_UDP", ""));
    if (!endpoint) {
        std::cerr << "pwctl: neither PW_HEARTBEAT_SOCKET nor PW_HEARTBEAT_UDP is set\n";
        return exit_usage;
    }
    pw::send_heartbeat_udp(*endpoint, datagram);
    return 0;
}

/// Parses arguments; returns an exit code when the program should stop.
[[nodiscard]] std::optional<int> parse_arguments(const std::vector<std::string_view>& args, Options& options)
{
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            std::cout << usage_text;
            return 0;
        }
        if (arg == "-V" || arg == "--version") {
            std::cout << "pwctl " << pw::version << "\n";
            return 0;
        }
        if (arg == "--raw") {
            options.raw = true;
        } else if (arg == "-s" || arg == "--socket" || arg == "-t" || arg == "--timeout") {
            if (i + 1 >= args.size()) {
                std::cerr << "pwctl: " << arg << " needs a value\n";
                return exit_usage;
            }
            const std::string_view value = args[++i];
            if (arg == "-s" || arg == "--socket") {
                options.socket = std::string(value);
            } else {
                const auto timeout = pw::parse_duration(value);
                if (!timeout || timeout->count() == 0) {
                    std::cerr << "pwctl: invalid timeout '" << value << "'\n";
                    return exit_usage;
                }
                options.timeout = *timeout;
            }
        } else if (!arg.empty() && arg.front() == '-') {
            std::cerr << "pwctl: unknown option '" << arg << "'\n";
            return exit_usage;
        } else {
            options.words.emplace_back(arg);
        }
    }
    if (options.words.empty() || options.words.size() > 2) {
        std::cerr << usage_text;
        return exit_usage;
    }
    if (options.socket.empty()) {
        options.socket = env_or("PW_CONTROL_SOCKET", "/tmp/pwatchd.sock");
    }
    return std::nullopt;
}

[[nodiscard]] int run(const Options& options)
{
    const std::string& verb = options.words.front();
    if (verb == "beat") {
        return send_beat(options);
    }
    std::string request = verb;
    if (options.words.size() > 1) {
        request += " " + options.words[1];
    }
    const pw::ControlResponse response = pw::send_control_request(options.socket, request, options.timeout);
    if (!response.ok) {
        std::cerr << "pwctl: " << response.message << "\n";
        return exit_rejected;
    }
    if (options.raw) {
        for (const std::string& line : response.body) {
            std::cout << line << "\n";
        }
    } else if (verb == "list") {
        print_table(response.body);
    } else if (verb == "status" && !response.body.empty()) {
        print_status(response.body.front());
    } else {
        if (!response.message.empty()) {
            std::cout << response.message << "\n";
        }
        for (const std::string& line : response.body) {
            std::cout << line << "\n";
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    Options options;
    try {
        const std::vector<std::string_view> args(argv + 1, argv + argc);
        if (const auto code = parse_arguments(args, options)) {
            return *code;
        }
        return run(options);
    } catch (const std::system_error& error) {
        std::cerr << "pwctl: " << error.what() << " (is pwatchd running? socket: " << options.socket << ")\n";
        return exit_connection;
    } catch (const std::exception& error) {
        std::cerr << "pwctl: " << error.what() << "\n";
        return exit_connection;
    }
}
