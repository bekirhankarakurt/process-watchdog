// pw-crasher - deliberately misbehaving child process for integration tests.
//
// Runs a sequence of actions given as arguments, e.g.
//   pw-crasher print:hello sleep:100 exit:3
//   pw-crasher beat:5:50 hang
//   pw-crasher ignore-term hang

#include "pw/config.hpp"
#include "pw/heartbeat.hpp"

#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

[[nodiscard]] std::string env(const char* name)
{
    const char* const value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
    return value == nullptr ? std::string{} : std::string(value);
}

[[nodiscard]] unsigned long to_number(std::string_view text)
{
    return std::stoul(std::string(text));
}

void sleep_ms(unsigned long ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void send_beat()
{
    const auto message = pw::parse_heartbeat("HB " + env("PW_SERVICE") + " " + env("PW_HEARTBEAT_TOKEN"));
    if (!message) {
        std::cerr << "crasher: missing heartbeat environment\n";
        std::exit(90);  // NOLINT(concurrency-mt-unsafe)
    }
    const std::string datagram = pw::format_heartbeat(message->service, message->token);
    if (!env("PW_HEARTBEAT_SOCKET").empty() && env("CRASHER_USE_UDP").empty()) {
        pw::send_heartbeat_unix(env("PW_HEARTBEAT_SOCKET"), datagram);
    } else if (const auto endpoint = pw::parse_endpoint(env("PW_HEARTBEAT_UDP"))) {
        pw::send_heartbeat_udp(*endpoint, datagram);
    }
}

[[noreturn]] void raise_signal(int number)
{
    std::fflush(stdout);
    std::signal(number, SIG_DFL);
    std::raise(number);
    std::_Exit(91);
}

void run_action(std::string_view action)
{
    const std::size_t colon = action.find(':');
    const std::string_view verb = action.substr(0, colon);
    const std::string_view arg = colon == std::string_view::npos ? std::string_view{} : action.substr(colon + 1);

    if (verb == "print") {
        std::cout << arg << std::endl;
    } else if (verb == "lines") {
        const unsigned long count = to_number(arg);
        for (unsigned long i = 0; i < count; ++i) {
            std::cout << "line " << i << " the quick brown fox jumps over the lazy dog\n";
        }
        std::cout.flush();
    } else if (verb == "sleep") {
        sleep_ms(to_number(arg));
    } else if (verb == "exit") {
        std::cout.flush();
        std::exit(static_cast<int>(to_number(arg)));  // NOLINT(concurrency-mt-unsafe)
    } else if (verb == "signal") {
        const auto number = pw::parse_signal(arg);
        raise_signal(number ? *number : SIGABRT);
    } else if (verb == "beat") {
        const std::size_t sep = arg.find(':');
        const unsigned long count = to_number(arg.substr(0, sep));
        const unsigned long interval = to_number(arg.substr(sep + 1));
        for (unsigned long i = 0; i < count; ++i) {
            send_beat();
            sleep_ms(interval);
        }
    } else if (verb == "ignore-term") {
        std::signal(SIGTERM, SIG_IGN);
    } else if (verb == "limits") {
        rlimit files{};
        rlimit core{};
        getrlimit(RLIMIT_NOFILE, &files);
        getrlimit(RLIMIT_CORE, &core);
        std::cout << "nofile=" << files.rlim_cur << " core=" << core.rlim_cur << std::endl;
    } else if (verb == "pidfile") {
        std::ofstream(std::string(arg)) << ::getpid() << "\n";
    } else if (verb == "hang") {
        for (;;) {
            ::pause();
        }
    } else {
        std::cerr << "crasher: unknown action '" << action << "'\n";
        std::exit(92);  // NOLINT(concurrency-mt-unsafe)
    }
}

}  // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string_view> actions(argv + 1, argv + argc);
    for (const std::string_view action : actions) {
        run_action(action);
    }
    return 0;
}
