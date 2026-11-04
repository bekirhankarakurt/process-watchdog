/// @file control.hpp
/// @brief Line-based control protocol shared by pwatchd and pwctl.
///
/// The client connects to the control socket, sends one request line and
/// reads until EOF. The first response line is `OK [message]` or
/// `ERR <message>`; following lines are the body. Service records are
/// tab-separated `key=value` fields.
#pragma once

#include "pw/util.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pw {

/// Ordered `key=value` record describing one service.
using Fields = std::vector<std::pair<std::string, std::string>>;

/// A parsed control request: a verb and an optional argument.
struct ControlRequest {
    std::string verb;
    std::string argument;
};

/// A parsed control response.
struct ControlResponse {
    bool ok{false};
    std::string message;
    std::vector<std::string> body;
};

/// Splits `verb [argument]`; returns empty for blank or over-long input.
[[nodiscard]] std::optional<ControlRequest> parse_request(std::string_view line);

/// Serialises a response, ending with a newline.
[[nodiscard]] std::string format_response(const ControlResponse& response);
/// Parses the full text received from the server.
[[nodiscard]] ControlResponse parse_response(std::string_view text);

/// Joins fields as `k=v<TAB>k=v`; tabs and newlines in values become spaces.
[[nodiscard]] std::string format_fields(const Fields& fields);
/// Inverse of format_fields(), preserving field order; items without `=` are skipped.
[[nodiscard]] Fields parse_fields_ordered(std::string_view line);
/// Inverse of format_fields() as a key lookup table.
[[nodiscard]] std::map<std::string, std::string> parse_fields(std::string_view line);

/// Connects to @p socket, sends @p request and returns the parsed reply.
/// @throws std::system_error on connection or I/O failure.
[[nodiscard]] ControlResponse send_control_request(const std::filesystem::path& socket,
                                                   std::string_view request, Millis timeout);

/// Maximum accepted request length in bytes.
inline constexpr std::size_t max_request_length = 512;

}  // namespace pw
