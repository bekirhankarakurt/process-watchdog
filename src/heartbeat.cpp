#include "pw/heartbeat.hpp"

#include "pw/fd.hpp"
#include "pw/socket.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <random>
#include <system_error>

namespace pw {
namespace {

void send_datagram(int fd, const sockaddr* address, socklen_t length, std::string_view datagram)
{
    const ssize_t sent = ::sendto(fd, datagram.data(), datagram.size(), MSG_NOSIGNAL, address, length);
    if (sent < 0) {
        throw std::system_error(errno, std::generic_category(), "sendto");
    }
}

}  // namespace

std::optional<HeartbeatMessage> parse_heartbeat(std::string_view datagram)
{
    datagram = trim(datagram);
    if (datagram.substr(0, 3) != "HB ") {
        return std::nullopt;
    }
    datagram.remove_prefix(3);
    const std::size_t space = datagram.find(' ');
    if (space == std::string_view::npos || space == 0) {
        return std::nullopt;
    }
    const std::string_view name = datagram.substr(0, space);
    const std::string_view hex = trim(datagram.substr(space + 1));
    if (hex.empty() || hex.size() > 16) {
        return std::nullopt;
    }
    std::uint64_t token = 0;
    const char* const end = hex.data() + hex.size();
    const auto [ptr, ec] = std::from_chars(hex.data(), end, token, 16);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return HeartbeatMessage{std::string(name), token};
}

std::string token_to_hex(std::uint64_t token)
{
    std::array<char, 16> buffer{};
    buffer.fill('0');
    std::array<char, 16> digits{};
    const auto [ptr, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), token, 16);
    static_cast<void>(ec);
    const auto count = static_cast<std::size_t>(ptr - digits.data());
    for (std::size_t i = 0; i < count; ++i) {
        buffer.at(buffer.size() - count + i) = digits.at(i);
    }
    return {buffer.data(), buffer.size()};
}

std::string format_heartbeat(std::string_view service, std::uint64_t token)
{
    return "HB " + std::string(service) + " " + token_to_hex(token) + "\n";
}

std::uint64_t make_token()
{
    std::random_device device;
    std::uint64_t token = 0;
    while (token == 0) {
        token = (static_cast<std::uint64_t>(device()) << 32U) ^ static_cast<std::uint64_t>(device());
    }
    return token;
}

void send_heartbeat_unix(const std::filesystem::path& socket, std::string_view datagram)
{
    const sockaddr_un address = make_unix_address(socket);
    const UniqueFd fd{::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0)};
    if (!fd.valid()) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }
    send_datagram(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address), datagram);
}

void send_heartbeat_udp(const UdpEndpoint& endpoint, std::string_view datagram)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
    if (inet_pton(AF_INET, endpoint.host.c_str(), &address.sin_addr) != 1) {
        throw std::system_error(std::make_error_code(std::errc::invalid_argument), "invalid address");
    }
    const UniqueFd fd{::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)};
    if (!fd.valid()) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }
    send_datagram(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address), datagram);
}

void HeartbeatMonitor::arm(TimePoint now) noexcept
{
    last_beat_.reset();
    beats_ = 0;
    if (spec_.enabled()) {
        deadline_ = now + (spec_.grace.count() > 0 ? spec_.grace : spec_.timeout);
    } else {
        deadline_.reset();
    }
}

void HeartbeatMonitor::beat(TimePoint now) noexcept
{
    last_beat_ = now;
    ++beats_;
    if (spec_.enabled() && deadline_) {
        deadline_ = now + spec_.timeout;
    }
}

void HeartbeatMonitor::disarm() noexcept
{
    deadline_.reset();
}

std::optional<TimePoint> HeartbeatMonitor::deadline() const noexcept
{
    return deadline_;
}

bool HeartbeatMonitor::expired(TimePoint now) const noexcept
{
    return deadline_ && now >= *deadline_;
}

}  // namespace pw
