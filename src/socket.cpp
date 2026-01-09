#include "pw/socket.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace pw {
namespace {

[[nodiscard]] std::system_error os_error(const std::string& what)
{
    return {errno, std::generic_category(), what};
}

/// Removes a leftover socket file, refusing if a server still answers on it.
void remove_stale_socket(const std::filesystem::path& path, int type)
{
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return;
    }
    if (!std::filesystem::is_socket(path, error)) {
        throw std::runtime_error(path.string() + " exists and is not a socket");
    }
    if (type == SOCK_STREAM) {
        const sockaddr_un address = make_unix_address(path);
        const UniqueFd probe{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
        if (probe.valid() &&
            ::connect(probe.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
            throw std::runtime_error("another instance is already listening on " + path.string());
        }
    }
    std::filesystem::remove(path, error);
}

[[nodiscard]] UniqueFd bind_unix(const std::filesystem::path& path, int type)
{
    const sockaddr_un address = make_unix_address(path);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    remove_stale_socket(path, type);
    UniqueFd fd{::socket(AF_UNIX, type | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
    if (!fd.valid()) {
        throw os_error("socket");
    }
    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        throw os_error("bind " + path.string());
    }
    if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        throw os_error("chmod " + path.string());
    }
    return fd;
}

}  // namespace

sockaddr_un make_unix_address(const std::filesystem::path& path)
{
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string& text = path.native();
    if (text.empty() || text.size() >= sizeof(address.sun_path)) {
        throw std::invalid_argument("UNIX socket path is empty or longer than " +
                                    std::to_string(sizeof(address.sun_path) - 1) + " bytes: " + text);
    }
    std::memcpy(static_cast<void*>(address.sun_path), text.c_str(), text.size() + 1);
    return address;
}

UniqueFd listen_unix_stream(const std::filesystem::path& path)
{
    UniqueFd fd = bind_unix(path, SOCK_STREAM);
    if (::listen(fd.get(), 16) != 0) {
        throw os_error("listen " + path.string());
    }
    return fd;
}

UniqueFd bind_unix_dgram(const std::filesystem::path& path)
{
    return bind_unix(path, SOCK_DGRAM);
}

UniqueFd bind_udp(const UdpEndpoint& endpoint)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
    if (inet_pton(AF_INET, endpoint.host.c_str(), &address.sin_addr) != 1) {
        throw std::invalid_argument("invalid IPv4 address: " + endpoint.host);
    }
    UniqueFd fd{::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
    if (!fd.valid()) {
        throw os_error("socket");
    }
    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        throw os_error("bind udp " + endpoint.host + ":" + std::to_string(endpoint.port));
    }
    return fd;
}

UniqueFd connect_unix_stream(const std::filesystem::path& path)
{
    const sockaddr_un address = make_unix_address(path);
    UniqueFd fd{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!fd.valid()) {
        throw os_error("socket");
    }
    if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        throw os_error("connect " + path.string());
    }
    return fd;
}

void set_socket_timeouts(int fd, Millis timeout)
{
    timeval value{};
    value.tv_sec = static_cast<decltype(value.tv_sec)>(timeout.count() / 1000);
    value.tv_usec = static_cast<decltype(value.tv_usec)>((timeout.count() % 1000) * 1000);
    static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)));
    static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value)));
}

}  // namespace pw
