/// @file socket.hpp
/// @brief Thin RAII wrappers for the UNIX and UDP sockets used by the watchdog.
#pragma once

#include "pw/config.hpp"
#include "pw/fd.hpp"

#include <sys/un.h>

#include <filesystem>

namespace pw {

/// Builds a `sockaddr_un`; throws std::invalid_argument if the path is too long.
[[nodiscard]] sockaddr_un make_unix_address(const std::filesystem::path& path);

/// Listening, non-blocking UNIX stream socket with mode 0600. A stale socket
/// file is replaced; a live one (another instance) raises std::runtime_error.
[[nodiscard]] UniqueFd listen_unix_stream(const std::filesystem::path& path);
/// Bound, non-blocking UNIX datagram socket (stale file replaced).
[[nodiscard]] UniqueFd bind_unix_dgram(const std::filesystem::path& path);
/// Bound, non-blocking UDP socket.
[[nodiscard]] UniqueFd bind_udp(const UdpEndpoint& endpoint);
/// Blocking client connection to a UNIX stream socket.
/// @throws std::system_error on failure.
[[nodiscard]] UniqueFd connect_unix_stream(const std::filesystem::path& path);

/// Sets SO_RCVTIMEO and SO_SNDTIMEO.
void set_socket_timeouts(int fd, Millis timeout);

}  // namespace pw
