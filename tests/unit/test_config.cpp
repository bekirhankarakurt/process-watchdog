#include "pw/config.hpp"
#include "test_support.hpp"

#include <csignal>
#include <string>

namespace {

const std::filesystem::path base{"/srv/app"};

pw::WatchdogConfig parse(const std::string& text)
{
    return pw::parse_config(text, base);
}

}  // namespace

TEST_CASE(full_configuration_is_parsed)
{
    const auto config = parse(R"(
[watchdog]
control_socket = run/ctl.sock
heartbeat_socket = /run/pw/hb.sock
heartbeat_udp = 127.0.0.1:9900
log_dir = /var/log/pw
log_level = debug

[service:api]
command = ./api --port "80 80"
working_dir = srv
env = MODE=prod
env = LEVEL=3
restart = always
backoff_initial = 100ms
backoff_max = 10s
backoff_multiplier = 1.5
max_restarts = unlimited
stable_after = 30s
heartbeat_timeout = 2s
heartbeat_grace = 15s
stop_signal = INT
stop_timeout = 3s
autostart = no
log_max_size = 1M
log_max_files = 3
log_timestamps = false
limit_nofile = 1024
limit_as = 512M
limit_cpu = 1m
limit_core = 0
)");
    CHECK_EQ(config.control_socket.string(), "/srv/app/run/ctl.sock");
    CHECK_EQ(config.heartbeat_socket->string(), "/run/pw/hb.sock");
    CHECK_EQ(config.heartbeat_udp->port, 9900U);
    CHECK(config.log_level == pw::LogLevel::Debug);
    const pw::ServiceConfig& api = config.services.at(0);
    CHECK_EQ(api.argv.size(), 3U);
    CHECK_EQ(api.argv[2], "80 80");
    CHECK_EQ(api.working_dir->string(), "/srv/app/srv");
    CHECK_EQ(api.env.size(), 2U);
    CHECK(api.restart.policy == pw::RestartPolicy::Always);
    CHECK_EQ(api.restart.backoff.initial.count(), 100);
    CHECK_EQ(api.restart.backoff.multiplier, 1.5);
    CHECK(!api.restart.max_restarts);
    CHECK_EQ(api.heartbeat.timeout.count(), 2000);
    CHECK_EQ(api.heartbeat.grace.count(), 15000);
    CHECK_EQ(api.stop_signal, SIGINT);
    CHECK(!api.autostart);
    CHECK_EQ(api.log.file.string(), "/var/log/pw/api.log");
    CHECK_EQ(api.log.max_bytes, 1024U * 1024U);
    CHECK(!api.log.timestamps);
    CHECK_EQ(api.limits.size(), 4U);
    CHECK(api.limits[2].kind == pw::LimitKind::CpuSeconds);
    CHECK_EQ(api.limits[2].value, 60U);
}

TEST_CASE(defaults_are_applied)
{
    const auto config = parse("[service:x]\ncommand = /bin/true\n");
    const pw::ServiceConfig& x = config.services.at(0);
    CHECK(x.restart.policy == pw::RestartPolicy::OnFailure);
    CHECK_EQ(*x.restart.max_restarts, 5U);
    CHECK(!x.heartbeat.enabled());
    CHECK_EQ(x.log.file.string(), "/srv/app/logs/x.log");
    CHECK_EQ(x.stop_signal, SIGTERM);
    CHECK(x.autostart);
}

TEST_CASE(heartbeat_grace_defaults_to_timeout)
{
    const auto config =
        parse("[watchdog]\nheartbeat_socket=/tmp/hb\n[service:x]\ncommand=/bin/true\nheartbeat_timeout=4s\n");
    CHECK_EQ(config.services[0].heartbeat.grace.count(), 4000);
}

TEST_CASE(invalid_configurations_are_rejected)
{
    CHECK_THROWS(pw::ConfigError, parse(""));
    CHECK_THROWS(pw::ConfigError, parse("[service:x]\n"));
    CHECK_THROWS(pw::ConfigError, parse("[service:x]\ncommand=a\nbogus=1\n"));
    CHECK_THROWS(pw::ConfigError, parse("[service:x]\ncommand=a\n[service:x]\ncommand=b\n"));
    CHECK_THROWS(pw::ConfigError, parse("[service:bad name]\ncommand=a\n"));
    CHECK_THROWS(pw::ConfigError, parse("[other]\n[service:x]\ncommand=a\n"));
    CHECK_THROWS(pw::ConfigError, parse("[service:x]\ncommand=a\nrestart=sometimes\n"));
    CHECK_THROWS(pw::ConfigError, parse("[service:x]\ncommand=a\nheartbeat_timeout=1s\n"));
    CHECK_THROWS(pw::ConfigError, parse("[service:x]\ncommand=a\nbackoff_initial=5s\nbackoff_max=1s\n"));
    CHECK_THROWS(pw::ConfigError, parse("[service:x]\ncommand='a\n"));
    CHECK_THROWS(pw::ConfigError, parse("[service:x]\ncommand=a\nstop_signal=NOPE\n"));
    CHECK_THROWS(pw::ConfigError, parse("[service:x]\ncommand=a\nenv=NOVALUE\n"));
    CHECK_THROWS(pw::ConfigError, parse("[service:x]\ncommand=a\nlimit_cpu=500ms\n"));
    CHECK_THROWS(pw::ConfigError, parse("[service:x]\ncommand=a\nbackoff_multiplier=0.5\n"));
    CHECK_THROWS(pw::ConfigError, parse("[watchdog]\nheartbeat_udp=nohost\n[service:x]\ncommand=a\n"));
    CHECK_THROWS(pw::ConfigError, parse("[watchdog]\n[watchdog]\n[service:x]\ncommand=a\n"));
}

TEST_CASE(endpoints_and_signals)
{
    CHECK_EQ(pw::parse_endpoint("localhost:80")->host, "127.0.0.1");
    CHECK(!pw::parse_endpoint("127.0.0.1:0"));
    CHECK(!pw::parse_endpoint("127.0.0.1:70000"));
    CHECK(!pw::parse_endpoint("example.com:80"));
    CHECK_EQ(*pw::parse_signal("SIGTERM"), SIGTERM);
    CHECK_EQ(*pw::parse_signal("kill"), SIGKILL);
    CHECK_EQ(*pw::parse_signal("10"), 10);
    CHECK(!pw::parse_signal("99"));
    CHECK_EQ(pw::signal_name(SIGSEGV), "SIGSEGV");
    CHECK_EQ(pw::signal_name(40), "SIG40");
    CHECK_EQ(pw::to_string(pw::RestartPolicy::OnFailure), "on-failure");
}

TEST_CASE(load_config_reports_missing_file)
{
    CHECK_THROWS(pw::ConfigError, pw::load_config("/nonexistent/pwatchd.ini"));
}

TEST_MAIN()
