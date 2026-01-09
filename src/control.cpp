#include "pw/control.hpp"

#include "pw/fd.hpp"
#include "pw/socket.hpp"

#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <system_error>
#include <utility>

namespace pw {

std::optional<ControlRequest> parse_request(std::string_view line)
{
    if (line.size() > max_request_length) {
        return std::nullopt;
    }
    line = trim(line);
    if (line.empty()) {
        return std::nullopt;
    }
    const std::size_t space = line.find_first_of(" \t");
    ControlRequest request;
    request.verb = to_lower(line.substr(0, space));
    if (space != std::string_view::npos) {
        request.argument = std::string(trim(line.substr(space + 1)));
    }
    return request;
}

std::string format_response(const ControlResponse& response)
{
    std::string text = response.ok ? "OK" : "ERR";
    if (!response.message.empty()) {
        text += " " + response.message;
    }
    text += "\n";
    for (const std::string& line : response.body) {
        text += line + "\n";
    }
    return text;
}

ControlResponse parse_response(std::string_view text)
{
    ControlResponse response;
    bool first = true;
    while (!text.empty()) {
        const std::size_t newline = text.find('\n');
        const std::string_view line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        if (first) {
            first = false;
            const std::size_t space = line.find(' ');
            const std::string_view status = line.substr(0, space);
            response.ok = status == "OK";
            if (!response.ok && status != "ERR") {
                response.message = "malformed response: " + std::string(line);
                return response;
            }
            if (space != std::string_view::npos) {
                response.message = std::string(line.substr(space + 1));
            }
        } else {
            response.body.emplace_back(line);
        }
    }
    if (first) {
        response.message = "empty response";
    }
    return response;
}

std::string format_fields(const Fields& fields)
{
    std::string line;
    for (const auto& [key, value] : fields) {
        if (!line.empty()) {
            line.push_back('\t');
        }
        line += key;
        line.push_back('=');
        for (const char c : value) {
            line.push_back(c == '\t' || c == '\n' || c == '\r' ? ' ' : c);
        }
    }
    return line;
}

Fields parse_fields_ordered(std::string_view line)
{
    Fields fields;
    while (!line.empty()) {
        const std::size_t tab = line.find('\t');
        const std::string_view item = line.substr(0, tab);
        line = tab == std::string_view::npos ? std::string_view{} : line.substr(tab + 1);
        const std::size_t equals = item.find('=');
        if (equals != std::string_view::npos) {
            fields.emplace_back(std::string(item.substr(0, equals)), std::string(item.substr(equals + 1)));
        }
    }
    return fields;
}

std::map<std::string, std::string> parse_fields(std::string_view line)
{
    std::map<std::string, std::string> table;
    for (auto& [key, value] : parse_fields_ordered(line)) {
        table[key] = std::move(value);
    }
    return table;
}

ControlResponse send_control_request(const std::filesystem::path& socket, std::string_view request,
                                     Millis timeout)
{
    const UniqueFd fd = connect_unix_stream(socket);
    set_socket_timeouts(fd.get(), timeout);
    std::string line(request);
    line.push_back('\n');
    std::string_view pending = line;
    while (!pending.empty()) {
        const ssize_t sent = ::send(fd.get(), pending.data(), pending.size(), MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "send");
        }
        pending.remove_prefix(static_cast<std::size_t>(sent));
    }
    static_cast<void>(::shutdown(fd.get(), SHUT_WR));

    std::string reply;
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t received = ::recv(fd.get(), buffer.data(), buffer.size(), 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "recv");
        }
        if (received == 0) {
            break;
        }
        reply.append(buffer.data(), static_cast<std::size_t>(received));
    }
    return parse_response(reply);
}

}  // namespace pw
