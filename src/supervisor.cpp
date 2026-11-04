#include "pw/supervisor.hpp"

#include "pw/control.hpp"
#include "pw/fd.hpp"
#include "pw/heartbeat.hpp"
#include "pw/logger.hpp"
#include "pw/process.hpp"
#include "pw/restart.hpp"
#include "pw/rotating_log.hpp"
#include "pw/socket.hpp"

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace pw {

std::string_view to_string(ServiceState state) noexcept
{
    switch (state) {
    case ServiceState::Stopped:
        return "stopped";
    case ServiceState::Running:
        return "running";
    case ServiceState::Stopping:
        return "stopping";
    case ServiceState::Backoff:
        return "backoff";
    case ServiceState::Exited:
        return "exited";
    case ServiceState::Failed:
        return "failed";
    }
    return "unknown";
}

namespace {

/// Identifies the origin of an epoll event; the low 32 bits hold an index.
enum class Source : std::uint32_t { Signal = 1, Control, HeartbeatUnix, HeartbeatUdp, Output };

[[nodiscard]] std::uint64_t make_tag(Source source, std::size_t index) noexcept
{
    return (static_cast<std::uint64_t>(source) << 32U) | static_cast<std::uint64_t>(index);
}

[[nodiscard]] std::system_error os_error(const std::string& what)
{
    return {errno, std::generic_category(), what};
}

[[nodiscard]] Millis elapsed(TimePoint since, TimePoint now) noexcept
{
    return std::chrono::duration_cast<Millis>(now - since);
}

/// What happens once the current process of a service has exited.
enum class AfterExit {
    Evaluate,  ///< Apply the restart policy (unexpected exit or heartbeat kill).
    Stop,      ///< Operator stop: stay stopped.
    Restart,   ///< Operator restart: start again immediately.
};

struct Service {
    explicit Service(ServiceConfig config)
        : cfg(std::move(config)), log(cfg.log), tracker(cfg.restart), heartbeat(cfg.heartbeat)
    {
    }

    ServiceConfig cfg;
    RotatingLog log;
    RestartTracker tracker;
    HeartbeatMonitor heartbeat;
    ServiceState state{ServiceState::Stopped};
    pid_t pid{-1};
    UniqueFd output;
    std::uint64_t token{0};
    unsigned restarts{0};
    TimePoint started_at;
    std::optional<TimePoint> wake_at;
    AfterExit after_exit{AfterExit::Evaluate};
    bool heartbeat_kill{false};
    std::string last_exit{"-"};
};

}  // namespace

class Supervisor::Impl {
public:
    explicit Impl(WatchdogConfig config) : config_(std::move(config))
    {
        set_log_level(config_.log_level);
        for (const ServiceConfig& service : config_.services) {
            services_.push_back(std::make_unique<Service>(service));
        }
        setup_signals();
        epoll_ = UniqueFd{::epoll_create1(EPOLL_CLOEXEC)};
        if (!epoll_.valid()) {
            throw os_error("epoll_create1");
        }
        watch(signal_fd_.get(), make_tag(Source::Signal, 0));

        control_fd_ = listen_unix_stream(config_.control_socket);
        watch(control_fd_.get(), make_tag(Source::Control, 0));
        log(LogLevel::Info, "control socket " + config_.control_socket.string());

        if (config_.heartbeat_socket) {
            heartbeat_unix_fd_ = bind_unix_dgram(*config_.heartbeat_socket);
            watch(heartbeat_unix_fd_.get(), make_tag(Source::HeartbeatUnix, 0));
            log(LogLevel::Info, "heartbeat socket " + config_.heartbeat_socket->string());
        }
        if (config_.heartbeat_udp) {
            heartbeat_udp_fd_ = bind_udp(*config_.heartbeat_udp);
            watch(heartbeat_udp_fd_.get(), make_tag(Source::HeartbeatUdp, 0));
            log(LogLevel::Info, "heartbeat udp " + config_.heartbeat_udp->host + ":" +
                                    std::to_string(config_.heartbeat_udp->port));
        }
    }

    ~Impl()
    {
        std::error_code ignored;
        std::filesystem::remove(config_.control_socket, ignored);
        if (config_.heartbeat_socket) {
            std::filesystem::remove(*config_.heartbeat_socket, ignored);
        }
        static_cast<void>(::sigprocmask(SIG_SETMASK, &previous_mask_, nullptr));
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    int run()
    {
        const TimePoint start = Clock::now();
        for (auto& service : services_) {
            if (service->cfg.autostart) {
                start_service(*service, start);
            }
        }
        std::array<epoll_event, 32> events{};
        while (!(shutting_down_ && all_down())) {
            const int ready = ::epoll_wait(epoll_.get(), events.data(), static_cast<int>(events.size()),
                                           next_timeout(Clock::now()));
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw os_error("epoll_wait");
            }
            for (std::size_t i = 0; i < static_cast<std::size_t>(ready); ++i) {
                dispatch(events.at(i));
            }
            handle_timers(Clock::now());
        }
        log(LogLevel::Info, "all services stopped, exiting");
        return 0;
    }

private:
    void setup_signals()
    {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGCHLD);
        sigaddset(&mask, SIGTERM);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGHUP);
        if (::sigprocmask(SIG_BLOCK, &mask, &previous_mask_) != 0) {
            throw os_error("sigprocmask");
        }
        signal_fd_ = UniqueFd{::signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK)};
        if (!signal_fd_.valid()) {
            throw os_error("signalfd");
        }
        static_cast<void>(std::signal(SIGPIPE, SIG_IGN));
    }

    void watch(int fd, std::uint64_t tag)
    {
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.u64 = tag;
        if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &event) != 0) {
            throw os_error("epoll_ctl");
        }
    }

    [[nodiscard]] bool all_down() const noexcept
    {
        return std::none_of(services_.begin(), services_.end(), [](const auto& service) {
            return service->state == ServiceState::Running || service->state == ServiceState::Stopping;
        });
    }

    [[nodiscard]] int next_timeout(TimePoint now) const noexcept
    {
        std::optional<TimePoint> nearest;
        const auto consider = [&nearest](std::optional<TimePoint> candidate) {
            if (candidate && (!nearest || *candidate < *nearest)) {
                nearest = candidate;
            }
        };
        for (const auto& service : services_) {
            consider(service->wake_at);
            if (service->state == ServiceState::Running) {
                consider(service->heartbeat.deadline());
            }
        }
        constexpr Millis::rep idle_ms = 60000;
        if (!nearest) {
            return static_cast<int>(idle_ms);
        }
        if (*nearest <= now) {
            return 0;
        }
        const auto wait = std::chrono::ceil<Millis>(*nearest - now).count();
        return static_cast<int>(std::min(wait, idle_ms));
    }

    void dispatch(const epoll_event& event)
    {
        const auto source = static_cast<Source>(event.data.u64 >> 32U);
        const auto index = static_cast<std::size_t>(event.data.u64 & 0xffffffffULL);
        switch (source) {
        case Source::Signal:
            handle_signals();
            break;
        case Source::Control:
            accept_control();
            break;
        case Source::HeartbeatUnix:
            receive_heartbeats(heartbeat_unix_fd_.get());
            break;
        case Source::HeartbeatUdp:
            receive_heartbeats(heartbeat_udp_fd_.get());
            break;
        case Source::Output:
            if (index < services_.size()) {
                drain_output(*services_.at(index));
            }
            break;
        }
    }

    // ---- signals and child exits -------------------------------------------------

    void handle_signals()
    {
        signalfd_siginfo info{};
        while (::read(signal_fd_.get(), &info, sizeof(info)) == static_cast<ssize_t>(sizeof(info))) {
            const auto number = static_cast<int>(info.ssi_signo);
            if (number == SIGCHLD) {
                reap_children();
            } else if (number == SIGHUP) {
                log(LogLevel::Info, "SIGHUP ignored (configuration reload is not supported)");
            } else {
                log(LogLevel::Info, "received " + signal_name(number) + ", shutting down");
                begin_shutdown(Clock::now());
            }
        }
    }

    void reap_children()
    {
        int status = 0;
        pid_t pid = 0;
        while ((pid = ::waitpid(-1, &status, WNOHANG)) > 0) {
            const auto it = std::find_if(services_.begin(), services_.end(),
                                         [pid](const auto& service) { return service->pid == pid; });
            if (it != services_.end()) {
                on_exit(**it, ExitStatus::from_wait_status(status), Clock::now());
            }
        }
    }

    void on_exit(Service& service, const ExitStatus& status, TimePoint now)
    {
        drain_output(service);
        close_output(service);
        service.heartbeat.disarm();
        service.wake_at.reset();
        service.pid = -1;

        const bool heartbeat_kill = std::exchange(service.heartbeat_kill, false);
        const bool failed = heartbeat_kill || !status.success();
        service.last_exit = status.describe() + (heartbeat_kill ? " after heartbeat timeout" : "");
        const Millis uptime = elapsed(service.started_at, now);
        log(failed && service.after_exit == AfterExit::Evaluate ? LogLevel::Warning : LogLevel::Info,
            service.cfg.name + ": exited (" + service.last_exit + ") after " + format_duration(uptime));

        if (shutting_down_) {
            service.state = ServiceState::Stopped;
            return;
        }
        switch (std::exchange(service.after_exit, AfterExit::Evaluate)) {
        case AfterExit::Stop:
            service.state = ServiceState::Stopped;
            return;
        case AfterExit::Restart:
            service.tracker.reset();
            spawn(service, now);
            return;
        case AfterExit::Evaluate:
            apply_policy(service, failed, uptime, now);
            return;
        }
    }

    static void apply_policy(Service& service, bool failed, Millis uptime, TimePoint now)
    {
        const RestartDecision decision = service.tracker.on_exit(failed, uptime);
        switch (decision.verdict) {
        case RestartVerdict::Restart:
            service.state = ServiceState::Backoff;
            service.wake_at = now + decision.delay;
            log(LogLevel::Info, service.cfg.name + ": restarting in " + format_duration(decision.delay) +
                                    " (attempt " + std::to_string(service.tracker.consecutive()) + ")");
            break;
        case RestartVerdict::Done:
            service.state = ServiceState::Exited;
            break;
        case RestartVerdict::GiveUp:
            service.state = ServiceState::Failed;
            log(LogLevel::Error, service.cfg.name + ": giving up after " +
                                     std::to_string(service.tracker.consecutive()) + " consecutive restarts");
            break;
        }
    }

    // ---- process lifecycle -------------------------------------------------------

    [[nodiscard]] std::vector<std::string> child_environment(const Service& service) const
    {
        std::vector<std::string> overrides{
            "PW_SERVICE=" + service.cfg.name,
            "PW_HEARTBEAT_TOKEN=" + token_to_hex(service.token),
            "PW_CONTROL_SOCKET=" + config_.control_socket.string(),
        };
        if (config_.heartbeat_socket) {
            overrides.push_back("PW_HEARTBEAT_SOCKET=" + config_.heartbeat_socket->string());
        }
        if (config_.heartbeat_udp) {
            overrides.push_back("PW_HEARTBEAT_UDP=" + config_.heartbeat_udp->host + ":" +
                                std::to_string(config_.heartbeat_udp->port));
        }
        overrides.insert(overrides.end(), service.cfg.env.begin(), service.cfg.env.end());
        return build_environment(overrides);
    }

    void spawn(Service& service, TimePoint now)
    {
        service.wake_at.reset();
        service.token = make_token();
        service.started_at = now;
        UniqueFd read_end;
        try {
            std::array<int, 2> pipe_fds{};
            if (::pipe2(pipe_fds.data(), O_CLOEXEC) != 0) {
                throw SpawnError(std::string("pipe2: ") + std::strerror(errno));
            }
            read_end.reset(pipe_fds[0]);
            const UniqueFd write_end{pipe_fds[1]};
            static_cast<void>(::fcntl(read_end.get(), F_SETFL, O_NONBLOCK));

            SpawnSpec spec;
            spec.argv = service.cfg.argv;
            spec.env = child_environment(service);
            spec.working_dir = service.cfg.working_dir;
            spec.limits = service.cfg.limits;
            spec.output_fd = write_end.get();
            service.pid = spawn_process(spec);
        } catch (const SpawnError& error) {
            service.last_exit = error.what();
            log(LogLevel::Error, service.cfg.name + ": " + service.last_exit);
            write_log(service, std::string("[pwatchd] ") + error.what() + "\n");
            apply_policy(service, true, Millis{0}, now);
            return;
        }
        service.output = std::move(read_end);
        watch(service.output.get(), make_tag(Source::Output, index_of(service)));
        service.state = ServiceState::Running;
        service.heartbeat.arm(now);
        log(LogLevel::Info, service.cfg.name + ": started pid " + std::to_string(service.pid));
    }

    void start_service(Service& service, TimePoint now)
    {
        service.tracker.reset();
        spawn(service, now);
    }

    static void begin_stop(Service& service, AfterExit after, TimePoint now)
    {
        service.after_exit = after;
        if (service.state == ServiceState::Stopping) {
            return;
        }
        service.state = ServiceState::Stopping;
        service.heartbeat.disarm();
        service.wake_at = now + service.cfg.stop_timeout;
        log(LogLevel::Info, service.cfg.name + ": sending " + signal_name(service.cfg.stop_signal) + " to pid " +
                                std::to_string(service.pid));
        signal_group(service, service.cfg.stop_signal);
    }

    static void signal_group(const Service& service, int signal_number) noexcept
    {
        if (service.pid <= 0) {
            return;
        }
        if (::kill(-service.pid, signal_number) != 0) {
            static_cast<void>(::kill(service.pid, signal_number));
        }
    }

    void begin_shutdown(TimePoint now)
    {
        shutting_down_ = true;
        for (auto& service : services_) {
            if (service->state == ServiceState::Running || service->state == ServiceState::Stopping) {
                begin_stop(*service, AfterExit::Stop, now);
            } else if (service->state == ServiceState::Backoff) {
                service->state = ServiceState::Stopped;
                service->wake_at.reset();
            }
        }
    }

    void handle_timers(TimePoint now)
    {
        for (auto& owned : services_) {
            Service& service = *owned;
            if (service.state == ServiceState::Backoff && service.wake_at && *service.wake_at <= now) {
                ++service.restarts;
                spawn(service, now);
            } else if (service.state == ServiceState::Stopping && service.wake_at && *service.wake_at <= now) {
                log(LogLevel::Warning, service.cfg.name + ": did not stop within " +
                                           format_duration(service.cfg.stop_timeout) + ", sending SIGKILL");
                service.wake_at.reset();
                signal_group(service, SIGKILL);
            } else if (service.state == ServiceState::Running && service.heartbeat.expired(now)) {
                log(LogLevel::Warning, service.cfg.name + ": heartbeat timeout (" +
                                           std::to_string(service.heartbeat.beats()) + " beats received)");
                service.heartbeat_kill = true;
                begin_stop(service, AfterExit::Evaluate, now);
            }
        }
    }

    // ---- output capture ----------------------------------------------------------

    void drain_output(Service& service)
    {
        if (!service.output.valid()) {
            return;
        }
        std::array<char, 16384> buffer{};
        for (;;) {
            const ssize_t count = ::read(service.output.get(), buffer.data(), buffer.size());
            if (count > 0) {
                write_log(service, std::string_view(buffer.data(), static_cast<std::size_t>(count)));
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                close_output(service);
            }
            return;
        }
    }

    void close_output(Service& service) noexcept
    {
        if (service.output.valid()) {
            static_cast<void>(::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, service.output.get(), nullptr));
            service.output.reset();
        }
    }

    static void write_log(Service& service, std::string_view data)
    {
        try {
            service.log.write(data);
        } catch (const std::exception& error) {
            log(LogLevel::Error, service.cfg.name + ": log write failed: " + error.what());
        }
    }

    // ---- heartbeats --------------------------------------------------------------

    void receive_heartbeats(int fd)
    {
        std::array<char, 512> buffer{};
        for (;;) {
            const ssize_t count = ::recv(fd, buffer.data(), buffer.size(), 0);
            if (count < 0) {
                return;
            }
            const auto message = parse_heartbeat(std::string_view(buffer.data(), static_cast<std::size_t>(count)));
            if (!message) {
                log(LogLevel::Debug, "ignoring malformed heartbeat datagram");
                continue;
            }
            Service* const service = find(message->service);
            if (service == nullptr || service->state != ServiceState::Running || service->token != message->token) {
                log(LogLevel::Debug, "rejected heartbeat for '" + message->service + "'");
                continue;
            }
            service->heartbeat.beat(Clock::now());
        }
    }

    // ---- control socket ----------------------------------------------------------

    void accept_control()
    {
        for (;;) {
            const UniqueFd client{::accept4(control_fd_.get(), nullptr, nullptr, SOCK_CLOEXEC)};
            if (!client.valid()) {
                return;
            }
            set_socket_timeouts(client.get(), Millis{1000});
            const std::optional<std::string> request = read_request(client.get());
            const ControlResponse response =
                request ? execute(*request, Clock::now()) : ControlResponse{false, "unreadable request", {}};
            const std::string reply = format_response(response);
            std::string_view pending = reply;
            while (!pending.empty()) {
                const ssize_t sent = ::send(client.get(), pending.data(), pending.size(), MSG_NOSIGNAL);
                if (sent <= 0) {
                    break;
                }
                pending.remove_prefix(static_cast<std::size_t>(sent));
            }
        }
    }

    [[nodiscard]] static std::optional<std::string> read_request(int fd)
    {
        std::string request;
        std::array<char, 256> buffer{};
        while (request.size() <= max_request_length) {
            const ssize_t count = ::recv(fd, buffer.data(), buffer.size(), 0);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                break;
            }
            request.append(buffer.data(), static_cast<std::size_t>(count));
            if (request.find('\n') != std::string::npos) {
                break;
            }
        }
        if (request.empty() || request.size() > max_request_length) {
            return std::nullopt;
        }
        return request.substr(0, request.find('\n'));
    }

    [[nodiscard]] ControlResponse execute(const std::string& line, TimePoint now)
    {
        const auto request = parse_request(line);
        if (!request) {
            return {false, "empty request", {}};
        }
        log(LogLevel::Debug, "control: " + line);
        const std::string& verb = request->verb;
        if (verb == "list") {
            ControlResponse response{true, std::to_string(services_.size()) + " services", {}};
            for (const auto& service : services_) {
                response.body.push_back(describe(*service, now));
            }
            return response;
        }
        if (verb == "status") {
            const Service* const service = find(request->argument);
            if (service == nullptr) {
                return {false, "unknown service '" + request->argument + "'", {}};
            }
            return {true, "", {describe(*service, now)}};
        }
        if (verb == "shutdown") {
            log(LogLevel::Info, "shutdown requested via control socket");
            begin_shutdown(now);
            return {true, "shutting down", {}};
        }
        if (verb == "start" || verb == "stop" || verb == "restart") {
            return apply_command(verb, request->argument, now);
        }
        return {false, "unknown command '" + verb + "' (try list, status, start, stop, restart, shutdown)", {}};
    }

    [[nodiscard]] ControlResponse apply_command(const std::string& verb, const std::string& target, TimePoint now)
    {
        if (target.empty()) {
            return {false, verb + " needs a service name or 'all'", {}};
        }
        if (shutting_down_) {
            return {false, "watchdog is shutting down", {}};
        }
        std::vector<Service*> selected;
        if (target == "all") {
            for (auto& service : services_) {
                selected.push_back(service.get());
            }
        } else if (Service* const service = find(target)) {
            selected.push_back(service);
        } else {
            return {false, "unknown service '" + target + "'", {}};
        }
        ControlResponse response{true, "", {}};
        for (Service* const service : selected) {
            response.body.push_back(service->cfg.name + ": " + command_one(verb, *service, now));
        }
        return response;
    }

    [[nodiscard]] std::string command_one(const std::string& verb, Service& service, TimePoint now)
    {
        const ServiceState state = service.state;
        if (verb == "start") {
            if (state == ServiceState::Running) {
                return "already running";
            }
            if (state == ServiceState::Stopping) {
                service.after_exit = AfterExit::Restart;
                return "will start after stopping";
            }
            start_service(service, now);
            return service.state == ServiceState::Running ? "started" : "start failed: " + service.last_exit;
        }
        if (verb == "stop") {
            if (state == ServiceState::Running || state == ServiceState::Stopping) {
                begin_stop(service, AfterExit::Stop, now);
                return "stopping";
            }
            if (state == ServiceState::Backoff) {
                service.state = ServiceState::Stopped;
                service.wake_at.reset();
                return "stopped";
            }
            return "not running";
        }
        if (state == ServiceState::Running || state == ServiceState::Stopping) {
            begin_stop(service, AfterExit::Restart, now);
            return "restarting";
        }
        start_service(service, now);
        return service.state == ServiceState::Running ? "started" : "start failed: " + service.last_exit;
    }

    [[nodiscard]] static std::string describe(const Service& service, TimePoint now)
    {
        const bool alive = service.state == ServiceState::Running || service.state == ServiceState::Stopping;
        std::string heartbeat = "-";
        if (service.cfg.heartbeat.enabled()) {
            const auto last = service.heartbeat.last_beat();
            heartbeat = last && alive ? format_duration(elapsed(*last, now)) : "none";
        }
        std::string next = "-";
        if (service.state == ServiceState::Backoff && service.wake_at) {
            next = format_duration(std::max(Millis{0}, elapsed(now, *service.wake_at)));
        }
        const Fields fields{
            {"name", service.cfg.name},
            {"state", std::string(to_string(service.state))},
            {"pid", alive ? std::to_string(service.pid) : "-"},
            {"restarts", std::to_string(service.restarts)},
            {"uptime", alive ? format_duration(elapsed(service.started_at, now)) : "-"},
            {"heartbeat", heartbeat},
            {"next_restart", next},
            {"policy", std::string(to_string(service.cfg.restart.policy))},
            {"last_exit", service.last_exit},
            {"log", service.log.path().string()},
        };
        return format_fields(fields);
    }

    [[nodiscard]] Service* find(std::string_view name) const noexcept
    {
        const auto it = std::find_if(services_.begin(), services_.end(),
                                     [name](const auto& service) { return service->cfg.name == name; });
        return it == services_.end() ? nullptr : it->get();
    }

    [[nodiscard]] std::size_t index_of(const Service& service) const noexcept
    {
        const auto it = std::find_if(services_.begin(), services_.end(),
                                     [&service](const auto& item) { return item.get() == &service; });
        return static_cast<std::size_t>(it - services_.begin());
    }

    WatchdogConfig config_;
    std::vector<std::unique_ptr<Service>> services_;
    sigset_t previous_mask_{};
    UniqueFd signal_fd_;
    UniqueFd epoll_;
    UniqueFd control_fd_;
    UniqueFd heartbeat_unix_fd_;
    UniqueFd heartbeat_udp_fd_;
    bool shutting_down_{false};
};

Supervisor::Supervisor(WatchdogConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

Supervisor::~Supervisor() = default;

int Supervisor::run()
{
    return impl_->run();
}

}  // namespace pw
