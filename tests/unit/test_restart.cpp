#include "pw/restart.hpp"
#include "test_support.hpp"

using pw::Millis;
using pw::RestartVerdict;

namespace {

pw::RestartSpec spec(pw::RestartPolicy policy, unsigned max)
{
    pw::RestartSpec result;
    result.policy = policy;
    result.max_restarts = max;
    result.backoff = {Millis{100}, Millis{1000}, 2.0};
    result.stable_after = Millis{5000};
    return result;
}

}  // namespace

TEST_CASE(backoff_grows_exponentially_and_caps)
{
    const pw::BackoffSpec backoff{Millis{100}, Millis{1000}, 2.0};
    CHECK_EQ(pw::backoff_delay(backoff, 0).count(), 100);
    CHECK_EQ(pw::backoff_delay(backoff, 1).count(), 200);
    CHECK_EQ(pw::backoff_delay(backoff, 3).count(), 800);
    CHECK_EQ(pw::backoff_delay(backoff, 4).count(), 1000);
    CHECK_EQ(pw::backoff_delay(backoff, 5000).count(), 1000);
}

TEST_CASE(policies_decide_on_failure_flag)
{
    CHECK(!pw::policy_wants_restart(pw::RestartPolicy::Never, true));
    CHECK(pw::policy_wants_restart(pw::RestartPolicy::OnFailure, true));
    CHECK(!pw::policy_wants_restart(pw::RestartPolicy::OnFailure, false));
    CHECK(pw::policy_wants_restart(pw::RestartPolicy::Always, false));
}

TEST_CASE(tracker_gives_up_after_max_restarts)
{
    pw::RestartTracker tracker(spec(pw::RestartPolicy::OnFailure, 3));
    for (unsigned i = 0; i < 3; ++i) {
        const auto decision = tracker.on_exit(true, Millis{10});
        CHECK(decision.verdict == RestartVerdict::Restart);
        CHECK_EQ(decision.delay.count(), 100 << i);
    }
    CHECK(tracker.on_exit(true, Millis{10}).verdict == RestartVerdict::GiveUp);
    CHECK_EQ(tracker.consecutive(), 3U);
}

TEST_CASE(stable_run_resets_counter_and_backoff)
{
    pw::RestartTracker tracker(spec(pw::RestartPolicy::OnFailure, 2));
    static_cast<void>(tracker.on_exit(true, Millis{10}));
    static_cast<void>(tracker.on_exit(true, Millis{10}));
    const auto decision = tracker.on_exit(true, Millis{6000});
    CHECK(decision.verdict == RestartVerdict::Restart);
    CHECK_EQ(decision.delay.count(), 100);
    CHECK_EQ(tracker.consecutive(), 1U);
}

TEST_CASE(clean_exit_is_done_for_on_failure)
{
    pw::RestartTracker tracker(spec(pw::RestartPolicy::OnFailure, 2));
    CHECK(tracker.on_exit(false, Millis{10}).verdict == RestartVerdict::Done);
    pw::RestartTracker always(spec(pw::RestartPolicy::Always, 2));
    CHECK(always.on_exit(false, Millis{10}).verdict == RestartVerdict::Restart);
}

TEST_CASE(unlimited_never_gives_up_and_reset_works)
{
    auto unlimited = spec(pw::RestartPolicy::Always, 0);
    unlimited.max_restarts.reset();
    pw::RestartTracker tracker(unlimited);
    for (int i = 0; i < 100; ++i) {
        CHECK(tracker.on_exit(true, Millis{0}).verdict == RestartVerdict::Restart);
    }
    tracker.reset();
    CHECK_EQ(tracker.consecutive(), 0U);
    pw::RestartTracker zero(spec(pw::RestartPolicy::OnFailure, 0));
    CHECK(zero.on_exit(true, Millis{0}).verdict == RestartVerdict::GiveUp);
}

TEST_MAIN()
