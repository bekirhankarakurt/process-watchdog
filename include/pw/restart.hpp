/// @file restart.hpp
/// @brief Pure restart-policy and backoff logic (no system calls).
#pragma once

#include "pw/config.hpp"
#include "pw/util.hpp"

namespace pw {

/// Outcome of evaluating an exited process against its restart policy.
enum class RestartVerdict {
    Restart,  ///< Restart after @ref RestartDecision::delay.
    Done,     ///< The policy does not ask for a restart.
    GiveUp,   ///< The policy asks for a restart but the limit is exhausted.
};

/// Verdict plus the backoff delay to apply when restarting.
struct RestartDecision {
    RestartVerdict verdict{RestartVerdict::Done};
    Millis delay{0};
};

/// Delay before restart number @p attempt (0-based), capped at the maximum.
[[nodiscard]] Millis backoff_delay(const BackoffSpec& spec, unsigned attempt) noexcept;

/// True if @p policy restarts a process that ended successfully or not.
[[nodiscard]] bool policy_wants_restart(RestartPolicy policy, bool failed) noexcept;

/// Tracks consecutive restarts of one service.
class RestartTracker {
public:
    explicit RestartTracker(RestartSpec spec) noexcept;

    /// Evaluates an exit. @p uptime is how long the process ran; a run longer
    /// than `stable_after` resets the counter before deciding.
    [[nodiscard]] RestartDecision on_exit(bool failed, Millis uptime) noexcept;

    /// Forgets previous restarts (used for manual start/restart).
    void reset() noexcept;

    /// Number of consecutive automatic restarts granted so far.
    [[nodiscard]] unsigned consecutive() const noexcept { return consecutive_; }

private:
    RestartSpec spec_;
    unsigned consecutive_{0};
};

}  // namespace pw
