#include "pw/util.hpp"
#include "test_support.hpp"

#include <stdexcept>

using pw::Millis;

TEST_CASE(trim_removes_surrounding_whitespace)
{
    CHECK_EQ(pw::trim("  \tabc \r\n"), "abc");
    CHECK_EQ(pw::trim(""), "");
    CHECK_EQ(pw::trim("   "), "");
}

TEST_CASE(split_command_handles_quotes_and_escapes)
{
    const auto words = pw::split_command(R"(sh -c 'echo "hi there"' "a\"b" c\ d)");
    CHECK_EQ(words.size(), 5U);
    CHECK_EQ(words[0], "sh");
    CHECK_EQ(words[2], "echo \"hi there\"");
    CHECK_EQ(words[3], "a\"b");
    CHECK_EQ(words[4], "c d");
    CHECK_EQ(pw::split_command("  ").size(), 0U);
    CHECK_EQ(pw::split_command("''").size(), 1U);
}

TEST_CASE(split_command_rejects_unterminated_input)
{
    CHECK_THROWS(std::invalid_argument, pw::split_command("echo 'oops"));
    CHECK_THROWS(std::invalid_argument, pw::split_command("echo \"oops"));
    CHECK_THROWS(std::invalid_argument, pw::split_command("echo oops\\"));
}

TEST_CASE(parse_uint_is_strict)
{
    CHECK_EQ(*pw::parse_uint("42"), 42U);
    CHECK(!pw::parse_uint("-1"));
    CHECK(!pw::parse_uint("4 2"));
    CHECK(!pw::parse_uint(""));
    CHECK(!pw::parse_uint("99999999999999999999999"));
}

TEST_CASE(parse_duration_units)
{
    CHECK_EQ(pw::parse_duration("250ms")->count(), 250);
    CHECK_EQ(pw::parse_duration("5s")->count(), 5000);
    CHECK_EQ(pw::parse_duration("5")->count(), 5000);
    CHECK_EQ(pw::parse_duration("2m")->count(), 120000);
    CHECK_EQ(pw::parse_duration("1h")->count(), 3600000);
    CHECK(!pw::parse_duration("5d"));
    CHECK(!pw::parse_duration("ms"));
    CHECK(!pw::parse_duration("-5s"));
}

TEST_CASE(parse_size_units)
{
    CHECK_EQ(*pw::parse_size("512"), 512U);
    CHECK_EQ(*pw::parse_size("64K"), 65536U);
    CHECK_EQ(*pw::parse_size("10M"), 10U * 1024U * 1024U);
    CHECK_EQ(*pw::parse_size("1g"), 1024ULL * 1024ULL * 1024ULL);
    CHECK(!pw::parse_size("1T"));
    CHECK(!pw::parse_size("99999999999999999G"));
}

TEST_CASE(parse_bool_and_double)
{
    CHECK(*pw::parse_bool("Yes"));
    CHECK(!*pw::parse_bool("off"));
    CHECK(!pw::parse_bool("maybe"));
    CHECK_EQ(*pw::parse_double("1.5"), 1.5);
    CHECK(!pw::parse_double("abc"));
    CHECK(!pw::parse_double("-2"));
}

TEST_CASE(format_duration_is_compact)
{
    CHECK_EQ(pw::format_duration(Millis{850}), "850ms");
    CHECK_EQ(pw::format_duration(Millis{12345}), "12.3s");
    CHECK_EQ(pw::format_duration(Millis{245000}), "4m05s");
    CHECK_EQ(pw::format_duration(Millis{7380000}), "2h03m");
    CHECK_EQ(pw::format_duration(Millis{(3 * 24 + 4) * 3600000LL}), "3d04h");
    CHECK_EQ(pw::format_duration(Millis{-5}), "0ms");
}

TEST_MAIN()
