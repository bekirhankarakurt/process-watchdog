/// @file supervisor.hpp
/// @brief The event loop that spawns, monitors and restarts services.
#pragma once

#include "pw/config.hpp"

#include <memory>
#include <string_view>

namespace pw {

/// Lifecycle state of a supervised service.
enum class ServiceState {
    Stopped,   ///< Not running; stopped by the operator or never started.
    Running,   ///< Process alive.
    Stopping,  ///< Stop signal sent, waiting for exit (SIGKILL after timeout).
    Backoff,   ///< Waiting for the backoff delay before an automatic restart.
    Exited,    ///< Ended and the restart policy does not restart it.
    Failed,    ///< Restart limit exhausted.
};

[[nodiscard]] std::string_view to_string(ServiceState state) noexcept;

/// Single-threaded epoll supervisor.
///
/// Signals (SIGCHLD, SIGTERM, SIGINT) are consumed through a signalfd; child
/// output, heartbeats and control requests are multiplexed on the same epoll
/// instance, and timers are derived from the nearest service deadline.
class Supervisor {
public:
    /// Binds sockets and opens log files.
    /// @throws std::exception if any resource cannot be set up.
    explicit Supervisor(WatchdogConfig config);
    ~Supervisor();

    Supervisor(const Supervisor&) = delete;
    Supervisor& operator=(const Supervisor&) = delete;
    Supervisor(Supervisor&&) = delete;
    Supervisor& operator=(Supervisor&&) = delete;

    /// Runs until SIGTERM, SIGINT or a `shutdown` request, then stops every
    /// service (escalating to SIGKILL) and returns the process exit code.
    [[nodiscard]] int run();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pw
