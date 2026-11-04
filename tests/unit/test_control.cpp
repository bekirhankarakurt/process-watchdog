#include "pw/control.hpp"
#include "test_support.hpp"

#include <string>

TEST_CASE(requests_are_parsed)
{
    const auto request = pw::parse_request("  STOP   api \n");
    CHECK(request.has_value());
    CHECK_EQ(request->verb, "stop");
    CHECK_EQ(request->argument, "api");
    CHECK_EQ(pw::parse_request("list")->argument, "");
    CHECK(!pw::parse_request("   "));
    CHECK(!pw::parse_request(std::string(pw::max_request_length + 1, 'x')));
}

TEST_CASE(responses_round_trip)
{
    const pw::ControlResponse response{true, "2 services", {"a", "b"}};
    const std::string text = pw::format_response(response);
    CHECK_EQ(text, "OK 2 services\na\nb\n");
    const auto parsed = pw::parse_response(text);
    CHECK(parsed.ok);
    CHECK_EQ(parsed.message, "2 services");
    CHECK_EQ(parsed.body.size(), 2U);

    const auto error = pw::parse_response(pw::format_response({false, "unknown service 'x'", {}}));
    CHECK(!error.ok);
    CHECK_EQ(error.message, "unknown service 'x'");
}

TEST_CASE(malformed_responses_are_not_ok)
{
    CHECK(!pw::parse_response("").ok);
    CHECK(!pw::parse_response("HELLO\n").ok);
    CHECK_EQ(pw::parse_response("").message, "empty response");
}

TEST_CASE(fields_round_trip_and_sanitize)
{
    const pw::Fields fields{{"name", "api"}, {"last_exit", "signal 11 (SIGSEGV)"}, {"bad", "a\tb\nc"}};
    const std::string line = pw::format_fields(fields);
    CHECK_EQ(line, "name=api\tlast_exit=signal 11 (SIGSEGV)\tbad=a b c");
    const auto parsed = pw::parse_fields(line + "\tnoequals");
    CHECK_EQ(parsed.at("name"), "api");
    CHECK_EQ(parsed.at("last_exit"), "signal 11 (SIGSEGV)");
    CHECK_EQ(parsed.count("noequals"), 0U);
    const auto ordered = pw::parse_fields_ordered(line);
    CHECK_EQ(ordered.size(), 3U);
    CHECK_EQ(ordered[1].first, "last_exit");
}

TEST_CASE(connecting_to_missing_socket_throws)
{
    CHECK_THROWS(std::system_error,
                 pw::send_control_request("/tmp/pw-no-such-socket.sock", "list", pw::Millis{100}));
}

TEST_MAIN()
