#include "pw/restart.hpp"

#include <cmath>

namespace pw {

Millis backoff_delay(const BackoffSpec& spec, unsigned attempt) noexcept
{
    const double initial = static_cast<double>(spec.initial.count());
    const double maximum = static_cast<double>(spec.max.count());
    const double scaled = initial * std::pow(spec.multiplier, static_cast<double>(attempt));
    if (!std::isfinite(scaled) || scaled >= maximum) {
        return spec.max;
    }
    return Millis{static_cast<Millis::rep>(scaled)};
}

bool policy_wants_restart(RestartPolicy policy, bool failed) noexcept
{
    switch (policy) {
    case RestartPolicy::Never:
        return false;
    case RestartPolicy::OnFailure:
        return failed;
    case RestartPolicy::Always:
        return true;
    }
    return false;
}

RestartTracker::RestartTracker(RestartSpec spec) noexcept : spec_(spec) {}

RestartDecision RestartTracker::on_exit(bool failed, Millis uptime) noexcept
{
    if (uptime >= spec_.stable_after) {
        consecutive_ = 0;
    }
    if (!policy_wants_restart(spec_.policy, failed)) {
        return {RestartVerdict::Done, Millis{0}};
    }
    if (spec_.max_restarts && consecutive_ >= *spec_.max_restarts) {
        return {RestartVerdict::GiveUp, Millis{0}};
    }
    const Millis delay = backoff_delay(spec_.backoff, consecutive_);
    ++consecutive_;
    return {RestartVerdict::Restart, delay};
}

void RestartTracker::reset() noexcept
{
    consecutive_ = 0;
}

}  // namespace pw
