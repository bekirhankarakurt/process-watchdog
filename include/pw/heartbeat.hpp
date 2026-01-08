/// @file heartbeat.hpp
/// @brief Heartbeat wire format, deadline tracking and sender helpers.
#pragma once

#include "pw/config.hpp"
#include "pw/util.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace pw {

/// A decoded heartbeat datagram: `HB <service> <token-hex>`.
struct HeartbeatMessage {
    std::string service;
    std::uint64_t token{0};
};

/// Decodes a datagram; returns empty for anything malformed.
[[nodiscard]] std::optional<HeartbeatMessage> parse_heartbeat(std::string_view datagram);
/// Encodes a heartbeat datagram (with trailing newline).
[[nodiscard]] std::string format_heartbeat(std::string_view service, std::uint64_t token);
/// Encodes @p token as 16 lower-case hex digits.
[[nodiscard]] std::string token_to_hex(std::uint64_t token);
/// Fresh non-zero random token issued to every spawned process.
[[nodiscard]] std::uint64_t make_token();

/// Sends one heartbeat datagram to a UNIX datagram socket.
/// @throws std::system_error on failure.
void send_heartbeat_unix(const std::filesystem::path& socket, std::string_view datagram);
/// Sends one heartbeat datagram over UDP.
/// @throws std::system_error on failure.
void send_heartbeat_udp(const UdpEndpoint& endpoint, std::string_view datagram);

/// Deadline bookkeeping for one running process.
class HeartbeatMonitor {
public:
    explicit HeartbeatMonitor(HeartbeatSpec spec) noexcept : spec_(spec) {}

    /// Starts supervision: the first beat is due within `grace` (or `timeout`).
    void arm(TimePoint now) noexcept;
    /// Records a beat and pushes the deadline to `now + timeout`.
    void beat(TimePoint now) noexcept;
    /// Stops supervision (process exited or is being stopped).
    void disarm() noexcept;

    /// Current deadline, empty when disabled or disarmed.
    [[nodiscard]] std::optional<TimePoint> deadline() const noexcept;
    /// True when armed and the deadline has passed.
    [[nodiscard]] bool expired(TimePoint now) const noexcept;
    /// Time of the last beat since the last arm(), if any.
    [[nodiscard]] std::optional<TimePoint> last_beat() const noexcept { return last_beat_; }
    /// Beats received since the last arm().
    [[nodiscard]] std::uint64_t beats() const noexcept { return beats_; }

private:
    HeartbeatSpec spec_;
    std::optional<TimePoint> deadline_;
    std::optional<TimePoint> last_beat_;
    std::uint64_t beats_{0};
};

}  // namespace pw
