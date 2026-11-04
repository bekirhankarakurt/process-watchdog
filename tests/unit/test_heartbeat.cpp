#include "pw/heartbeat.hpp"
#include "pw/socket.hpp"
#include "test_support.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <string>

using pw::Millis;

TEST_CASE(message_round_trip)
{
    const std::string datagram = pw::format_heartbeat("api", 0xabcULL);
    CHECK_EQ(datagram, "HB api 0000000000000abc\n");
    const auto message = pw::parse_heartbeat(datagram);
    CHECK(message.has_value());
    CHECK_EQ(message->service, "api");
    CHECK_EQ(message->token, 0xabcULL);
    CHECK_EQ(pw::token_to_hex(~0ULL), "ffffffffffffffff");
}

TEST_CASE(malformed_messages_are_rejected)
{
    CHECK(!pw::parse_heartbeat(""));
    CHECK(!pw::parse_heartbeat("HB"));
    CHECK(!pw::parse_heartbeat("HB api"));
    CHECK(!pw::parse_heartbeat("HB api xyz"));
    CHECK(!pw::parse_heartbeat("HB api 00000000000000000"));
    CHECK(!pw::parse_heartbeat("PING api 1"));
}

TEST_CASE(tokens_are_random_and_nonzero)
{
    const auto a = pw::make_token();
    const auto b = pw::make_token();
    CHECK(a != 0);
    CHECK(a != b);
}

TEST_CASE(monitor_uses_grace_then_timeout)
{
    pw::HeartbeatMonitor monitor(pw::HeartbeatSpec{Millis{100}, Millis{1000}});
    const pw::TimePoint t0{};
    CHECK(!monitor.deadline());
    monitor.arm(t0);
    CHECK(*monitor.deadline() == t0 + Millis{1000});
    CHECK(!monitor.expired(t0 + Millis{999}));
    CHECK(monitor.expired(t0 + Millis{1000}));
    monitor.beat(t0 + Millis{500});
    CHECK(*monitor.deadline() == t0 + Millis{600});
    CHECK_EQ(monitor.beats(), 1U);
    CHECK(*monitor.last_beat() == t0 + Millis{500});
    monitor.disarm();
    CHECK(!monitor.expired(t0 + Millis{5000}));
    monitor.beat(t0 + Millis{5000});
    CHECK(!monitor.deadline());
}

TEST_CASE(disabled_monitor_never_expires)
{
    pw::HeartbeatMonitor monitor(pw::HeartbeatSpec{});
    monitor.arm(pw::TimePoint{});
    CHECK(!monitor.deadline());
    CHECK(!monitor.expired(pw::TimePoint{} + Millis{100000}));
}

TEST_CASE(unix_and_udp_datagrams_are_delivered)
{
    const std::string path = "/tmp/pw-hb-test-" + std::to_string(::getpid()) + ".sock";
    const pw::UniqueFd server = pw::bind_unix_dgram(path);
    pw::send_heartbeat_unix(path, pw::format_heartbeat("a", 1));
    std::array<char, 128> buffer{};
    const ssize_t received = ::recv(server.get(), buffer.data(), buffer.size(), 0);
    CHECK(received > 0);
    CHECK_EQ(pw::parse_heartbeat(std::string(buffer.data(), static_cast<std::size_t>(received)))->service, "a");
    ::unlink(path.c_str());

    const pw::UniqueFd udp = pw::bind_udp(pw::UdpEndpoint{"127.0.0.1", 0});
    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    CHECK_EQ(::getsockname(udp.get(), reinterpret_cast<sockaddr*>(&bound), &length), 0);
    pw::send_heartbeat_udp(pw::UdpEndpoint{"127.0.0.1", ntohs(bound.sin_port)}, pw::format_heartbeat("b", 2));
    const ssize_t udp_received = ::recv(udp.get(), buffer.data(), buffer.size(), 0);
    CHECK(udp_received > 0);
    CHECK_EQ(pw::parse_heartbeat(std::string(buffer.data(), static_cast<std::size_t>(udp_received)))->token, 2U);
}

TEST_MAIN()
