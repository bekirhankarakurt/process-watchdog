// End-to-end tests: run the real pwatchd binary against deliberately
// misbehaving children (pw-crasher) and inspect it through the control socket.
//
// Usage: integration_tests SCENARIO PWATCHD CRASHER PWCTL

#include "pw/control.hpp"
#include "pw/fd.hpp"
#include "pw/heartbeat.hpp"
#include "pw/process.hpp"
#include "pw/socket.hpp"
#include "test_support.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

struct Binaries {
    std::string pwatchd;
    std::string crasher;
    std::string pwctl;
};

Binaries& binaries()
{
    static Binaries paths;
    return paths;
}

std::string read_file(const fs::path& path)
{
    std::ifstream stream(path);
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}

std::size_t count_occurrences(const std::string& text, const std::string& needle)
{
    std::size_t count = 0;
    for (std::size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + 1)) {
        ++count;
    }
    return count;
}

void replace_all(std::string& text, const std::string& from, const std::string& to)
{
    for (std::size_t pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos + to.size())) {
        text.replace(pos, from.size(), to);
    }
}

bool wait_until(const std::function<bool()>& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(20ms);
    }
    return predicate();
}

/// Runs a command to completion and captures stdout+stderr.
std::pair<pw::ExitStatus, std::string> run_capture(const std::vector<std::string>& argv,
                                                   const std::vector<std::string>& env_overrides = {})
{
    std::array<int, 2> fds{};
    CHECK_EQ(::pipe2(fds.data(), O_CLOEXEC), 0);
    pw::UniqueFd read_end{fds[0]};
    pw::UniqueFd write_end{fds[1]};
    pw::SpawnSpec spec;
    spec.argv = argv;
    spec.env = pw::build_environment(env_overrides);
    spec.output_fd = write_end.get();
    const pid_t pid = pw::spawn_process(spec);
    write_end.reset();
    std::string output;
    std::array<char, 1024> buffer{};
    ssize_t count = 0;
    while ((count = ::read(read_end.get(), buffer.data(), buffer.size())) > 0) {
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return {pw::ExitStatus::from_wait_status(status), output};
}

/// A pwatchd instance running in its own temporary directory.
class Watchdog {
public:
    explicit Watchdog(std::string services, const std::string& extra_watchdog = "")
    {
        std::string pattern = "/tmp/pw-it-XXXXXX";
        CHECK(::mkdtemp(pattern.data()) != nullptr);
        dir_ = pattern;
        replace_all(services, "{crasher}", binaries().crasher);
        replace_all(services, "{pwctl}", binaries().pwctl);
        replace_all(services, "{dir}", dir_.string());
        std::ofstream(dir_ / "watchdog.ini") << "[watchdog]\ncontrol_socket = ctl.sock\nheartbeat_socket = hb.sock\n"
                                             << "log_dir = logs\nlog_level = debug\n"
                                             << extra_watchdog << "\n"
                                             << services;

        const pw::UniqueFd out{::open((dir_ / "pwatchd.out").c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644)};
        pw::SpawnSpec spec;
        spec.argv = {binaries().pwatchd, "-c", (dir_ / "watchdog.ini").string()};
        spec.env = pw::build_environment({});
        spec.output_fd = out.get();
        pid_ = pw::spawn_process(spec);
        const bool up = wait_until(
            [&] {
                try {
                    return request("list").ok;
                } catch (const std::exception&) {
                    return false;
                }
            },
            10s);
        if (!up) {
            dump();
            throw pwtest::Failure("pwatchd did not come up");
        }
    }

    ~Watchdog()
    {
        if (std::uncaught_exceptions() > 0) {
            dump();
        }
        if (pid_ > 0) {
            stop_daemon();
        }
        std::error_code ignored;
        fs::remove_all(dir_, ignored);
    }

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;
    Watchdog(Watchdog&&) = delete;
    Watchdog& operator=(Watchdog&&) = delete;

    [[nodiscard]] pw::ControlResponse request(const std::string& line) const
    {
        return pw::send_control_request(socket(), line, 5s);
    }

    [[nodiscard]] std::map<std::string, std::string> status(const std::string& name) const
    {
        const auto response = request("status " + name);
        CHECK(response.ok);
        CHECK_EQ(response.body.size(), 1U);
        return pw::parse_fields(response.body.front());
    }

    [[nodiscard]] std::string field(const std::string& name, const std::string& key) const
    {
        return status(name).at(key);
    }

    void expect_state(const std::string& name, const std::string& state, std::chrono::milliseconds timeout) const
    {
        if (!wait_until([&] { return field(name, "state") == state; }, timeout)) {
            throw pwtest::Failure("service '" + name + "' did not reach state '" + state + "' (now '" +
                                  field(name, "state") + "', last_exit '" + field(name, "last_exit") + "')");
        }
    }

    /// Requests shutdown and returns pwatchd's exit status.
    pw::ExitStatus stop_daemon()
    {
        try {
            static_cast<void>(request("shutdown"));
        } catch (const std::exception&) {
            static_cast<void>(::kill(pid_, SIGTERM));
        }
        int status = 0;
        const bool exited = wait_until([&] { return ::waitpid(pid_, &status, WNOHANG) == pid_; }, 15s);
        if (!exited) {
            static_cast<void>(::kill(pid_, SIGKILL));
            static_cast<void>(::waitpid(pid_, &status, 0));
        }
        pid_ = -1;
        return pw::ExitStatus::from_wait_status(status);
    }

    [[nodiscard]] fs::path socket() const { return dir_ / "ctl.sock"; }
    [[nodiscard]] const fs::path& dir() const { return dir_; }
    [[nodiscard]] std::string service_log(const std::string& name) const
    {
        return read_file(dir_ / "logs" / (name + ".log"));
    }

private:
    void dump() const
    {
        std::cerr << "---- pwatchd output (" << dir_ << ") ----\n" << read_file(dir_ / "pwatchd.out") << "----\n";
    }

    fs::path dir_;
    pid_t pid_{-1};
};

int to_int(const std::string& text)
{
    return std::stoi(text);
}

bool contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

// ---- scenarios -------------------------------------------------------------------

TEST_CASE(crash_loop_gives_up)
{
    const Watchdog watchdog(R"(
[service:crashy]
command = {crasher} print:boom exit:3
max_restarts = 3
backoff_initial = 20ms
backoff_max = 80ms
)");
    watchdog.expect_state("crashy", "failed", 10s);
    const auto status = watchdog.status("crashy");
    CHECK_EQ(status.at("restarts"), "3");
    CHECK_EQ(status.at("last_exit"), "exit 3");
    CHECK_EQ(status.at("pid"), "-");
    CHECK_EQ(count_occurrences(watchdog.service_log("crashy"), "boom"), 4U);
}

TEST_CASE(clean_exit_not_restarted)
{
    const Watchdog watchdog("[service:job]\ncommand = {crasher} sleep:50 exit:0\n");
    watchdog.expect_state("job", "exited", 10s);
    CHECK_EQ(watchdog.field("job", "restarts"), "0");
    CHECK_EQ(watchdog.field("job", "last_exit"), "exit 0");
}

TEST_CASE(always_policy_restarts_clean_exit)
{
    const Watchdog watchdog(R"(
[service:loop]
command = {crasher} sleep:10 exit:0
restart = always
max_restarts = unlimited
backoff_initial = 10ms
backoff_max = 20ms
)");
    CHECK(wait_until([&] { return to_int(watchdog.field("loop", "restarts")) >= 3; }, 10s));
}

TEST_CASE(segfault_is_reported)
{
    const Watchdog watchdog("[service:segv]\ncommand = {crasher} signal:SEGV\nmax_restarts = 0\n");
    watchdog.expect_state("segv", "failed", 10s);
    CHECK_EQ(watchdog.field("segv", "last_exit"), "signal 11 (SIGSEGV)");
}

TEST_CASE(backoff_grows_between_restarts)
{
    const auto start = std::chrono::steady_clock::now();
    const Watchdog watchdog(R"(
[service:flaky]
command = {crasher} exit:1
max_restarts = 3
backoff_initial = 100ms
backoff_multiplier = 2
backoff_max = 1s
)");
    CHECK(wait_until([&] { return watchdog.field("flaky", "state") == "backoff"; }, 5s));
    watchdog.expect_state("flaky", "failed", 10s);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed >= 650ms);  // 100 + 200 + 400 ms of backoff
}

TEST_CASE(heartbeat_keeps_service_alive)
{
    const Watchdog watchdog(R"(
[service:beating]
command = {crasher} beat:30:50 exit:0
heartbeat_timeout = 400ms
)");
    std::this_thread::sleep_for(1200ms);
    const auto status = watchdog.status("beating");
    CHECK_EQ(status.at("state"), "running");
    CHECK_EQ(status.at("restarts"), "0");
    CHECK(status.at("heartbeat") != "none");
    watchdog.expect_state("beating", "exited", 10s);
}

TEST_CASE(heartbeat_timeout_restarts)
{
    const Watchdog watchdog(R"(
[service:stuck]
command = {crasher} beat:3:50 hang
heartbeat_timeout = 300ms
max_restarts = 1
backoff_initial = 20ms
backoff_max = 20ms
)");
    watchdog.expect_state("stuck", "failed", 10s);
    CHECK_EQ(watchdog.field("stuck", "restarts"), "1");
    CHECK(contains(watchdog.field("stuck", "last_exit"), "after heartbeat timeout"));
}

TEST_CASE(heartbeat_wrong_token_ignored)
{
    const Watchdog watchdog(R"(
[service:target]
command = {crasher} hang
heartbeat_timeout = 400ms
max_restarts = 0
)");
    const fs::path hb = watchdog.dir() / "hb.sock";
    for (int i = 0; i < 15 && watchdog.field("target", "state") == "running"; ++i) {
        pw::send_heartbeat_unix(hb, pw::format_heartbeat("target", 0x1234));
        pw::send_heartbeat_unix(hb, pw::format_heartbeat("unknown", 0x1234));
        pw::send_heartbeat_unix(hb, "garbage");
        std::this_thread::sleep_for(50ms);
    }
    watchdog.expect_state("target", "failed", 5s);
    CHECK(contains(watchdog.field("target", "last_exit"), "heartbeat timeout"));
}

TEST_CASE(heartbeat_over_udp)
{
    std::string port;
    {
        const pw::UniqueFd probe = pw::bind_udp(pw::UdpEndpoint{"127.0.0.1", 0});
        sockaddr_in bound{};
        socklen_t length = sizeof(bound);
        CHECK_EQ(::getsockname(probe.get(), reinterpret_cast<sockaddr*>(&bound), &length), 0);
        port = std::to_string(ntohs(bound.sin_port));
    }
    const Watchdog watchdog(R"(
[service:udp]
command = {crasher} beat:30:50 exit:0
env = CRASHER_USE_UDP=1
heartbeat_timeout = 400ms
max_restarts = 0
)",
                            "heartbeat_udp = 127.0.0.1:" + port);
    std::this_thread::sleep_for(1000ms);
    CHECK_EQ(watchdog.field("udp", "state"), "running");
    CHECK(watchdog.field("udp", "heartbeat") != "none");
    watchdog.expect_state("udp", "exited", 10s);
}

TEST_CASE(stop_escalates_to_sigkill)
{
    const Watchdog watchdog(R"(
[service:stubborn]
command = {crasher} ignore-term print:ready hang
stop_timeout = 300ms
log_timestamps = false
)");
    CHECK(wait_until([&] { return contains(watchdog.service_log("stubborn"), "ready"); }, 5s));
    const auto start = std::chrono::steady_clock::now();
    const auto response = watchdog.request("stop stubborn");
    CHECK(response.ok);
    CHECK_EQ(response.body.at(0), "stubborn: stopping");
    watchdog.expect_state("stubborn", "stopped", 5s);
    CHECK(std::chrono::steady_clock::now() - start >= 300ms);
    CHECK_EQ(watchdog.field("stubborn", "last_exit"), "signal 9 (SIGKILL)");
    CHECK_EQ(watchdog.field("stubborn", "restarts"), "0");
}

TEST_CASE(control_start_stop_restart)
{
    const Watchdog watchdog(R"(
[service:svc]
command = {crasher} hang
autostart = false

[service:other]
command = {crasher} hang
)");
    CHECK_EQ(watchdog.field("svc", "state"), "stopped");
    CHECK_EQ(watchdog.request("start svc").body.at(0), "svc: started");
    const std::string first_pid = watchdog.field("svc", "pid");
    CHECK(first_pid != "-");
    CHECK_EQ(watchdog.request("start svc").body.at(0), "svc: already running");

    CHECK_EQ(watchdog.request("restart svc").body.at(0), "svc: restarting");
    CHECK(wait_until(
        [&] { return watchdog.field("svc", "state") == "running" && watchdog.field("svc", "pid") != first_pid; },
        5s));
    CHECK_EQ(watchdog.field("svc", "restarts"), "0");

    const auto stop_all = watchdog.request("stop all");
    CHECK(stop_all.ok);
    CHECK_EQ(stop_all.body.size(), 2U);
    watchdog.expect_state("svc", "stopped", 5s);
    watchdog.expect_state("other", "stopped", 5s);
    CHECK_EQ(watchdog.request("stop svc").body.at(0), "svc: not running");
    CHECK(watchdog.request("start all").ok);
    watchdog.expect_state("other", "running", 5s);
}

TEST_CASE(control_rejects_bad_requests)
{
    const Watchdog watchdog("[service:svc]\ncommand = {crasher} hang\n");
    const auto list = watchdog.request("list");
    CHECK(list.ok);
    CHECK_EQ(list.message, "1 services");
    CHECK_EQ(list.body.size(), 1U);
    CHECK(!watchdog.request("bogus").ok);
    CHECK(!watchdog.request("stop nope").ok);
    CHECK(!watchdog.request("start").ok);
    CHECK(!watchdog.request("status nope").ok);
    CHECK(!watchdog.request(std::string(600, 'x')).ok);
    CHECK_EQ(watchdog.field("svc", "state"), "running");
}

TEST_CASE(log_capture_and_rotation)
{
    const Watchdog watchdog(R"(
[service:chatty]
command = {crasher} lines:200 print:done exit:0
log_max_size = 2K
log_max_files = 2
log_timestamps = false
)");
    watchdog.expect_state("chatty", "exited", 10s);
    const fs::path log = watchdog.dir() / "logs" / "chatty.log";
    CHECK(fs::exists(log));
    CHECK(fs::exists(log.string() + ".1"));
    CHECK(fs::exists(log.string() + ".2"));
    CHECK(!fs::exists(log.string() + ".3"));
    CHECK(fs::file_size(log) <= 16384U);
    CHECK(contains(watchdog.service_log("chatty"), "done"));
}

TEST_CASE(resource_limits_applied)
{
    const Watchdog watchdog(R"(
[service:limited]
command = {crasher} limits exit:0
limit_nofile = 64
limit_core = 0
)");
    watchdog.expect_state("limited", "exited", 10s);
    CHECK(contains(watchdog.service_log("limited"), "nofile=64 core=0"));
}

TEST_CASE(missing_binary_fails)
{
    const Watchdog watchdog(R"(
[service:ghost]
command = /nonexistent/program --flag
max_restarts = 1
backoff_initial = 20ms
backoff_max = 20ms
)");
    watchdog.expect_state("ghost", "failed", 10s);
    CHECK_EQ(watchdog.field("ghost", "restarts"), "1");
    CHECK(contains(watchdog.field("ghost", "last_exit"), "No such file"));
    CHECK(contains(watchdog.service_log("ghost"), "No such file"));
}

TEST_CASE(shutdown_stops_children)
{
    Watchdog watchdog(R"(
[service:plain]
command = {crasher} pidfile:{dir}/plain.pid hang

[service:stubborn]
command = {crasher} ignore-term pidfile:{dir}/stubborn.pid hang
stop_timeout = 300ms
)");
    const fs::path plain = watchdog.dir() / "plain.pid";
    const fs::path stubborn = watchdog.dir() / "stubborn.pid";
    CHECK(wait_until([&] { return fs::exists(plain) && fs::exists(stubborn) && fs::file_size(stubborn) > 0; }, 5s));
    const int plain_pid = to_int(read_file(plain));
    const int stubborn_pid = to_int(read_file(stubborn));
    const pw::ExitStatus status = watchdog.stop_daemon();
    CHECK(status.success());
    CHECK(::kill(plain_pid, 0) != 0);
    CHECK(::kill(stubborn_pid, 0) != 0);
    CHECK(!fs::exists(watchdog.socket()));
}

TEST_CASE(pwctl_cli)
{
    const Watchdog watchdog(R"(
[service:shell]
command = /bin/sh -c 'while true; do {pwctl} beat || exit 1; sleep 0.1; done'
heartbeat_timeout = 500ms
)");
    const std::string socket = watchdog.socket().string();
    const auto [list_status, list_output] = run_capture({binaries().pwctl, "-s", socket, "list"});
    CHECK(list_status.success());
    CHECK(contains(list_output, "NAME"));
    CHECK(contains(list_output, "shell"));

    std::this_thread::sleep_for(1500ms);
    const auto [status_status, status_output] = run_capture({binaries().pwctl, "status", "shell"},
                                                            {"PW_CONTROL_SOCKET=" + socket});
    CHECK(status_status.success());
    CHECK(contains(status_output, "state:        running"));
    CHECK(contains(status_output, "restarts:     0"));

    CHECK_EQ(run_capture({binaries().pwctl, "-s", socket, "stop", "nope"}).first.value, 1);
    CHECK_EQ(run_capture({binaries().pwctl, "-s", "/tmp/pw-none.sock", "list"}).first.value, 3);
    CHECK_EQ(run_capture({binaries().pwctl, "frobnicate", "a", "b"}).first.value, 2);
    CHECK_EQ(run_capture({binaries().pwctl, "beat"}, {"PW_SERVICE=", "PW_HEARTBEAT_TOKEN="}).first.value, 2);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 5) {
        std::cerr << "usage: integration_tests SCENARIO PWATCHD CRASHER PWCTL\n";
        return 2;
    }
    binaries() = Binaries{argv[2], argv[3], argv[4]};
    std::array<char*, 2> filter{argv[0], argv[1]};
    return pwtest::run_all(2, filter.data());
}
