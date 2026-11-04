#include "pw/ini.hpp"
#include "test_support.hpp"

TEST_CASE(parses_sections_entries_and_comments)
{
    const auto sections = pw::parse_ini("# comment\n[a]\nkey = value with = sign\n; other\n\n[ b ]\nx=1\nx=2\n");
    CHECK_EQ(sections.size(), 2U);
    CHECK_EQ(sections[0].name, "a");
    CHECK_EQ(sections[0].entries[0].key, "key");
    CHECK_EQ(sections[0].entries[0].value, "value with = sign");
    CHECK_EQ(sections[0].entries[0].line, 3);
    CHECK_EQ(sections[1].name, "b");
    CHECK_EQ(sections[1].entries.size(), 2U);
    CHECK_EQ(sections[1].entries[1].value, "2");
}

TEST_CASE(handles_crlf_and_empty_values)
{
    const auto sections = pw::parse_ini("[s]\r\nk =\r\n");
    CHECK_EQ(sections[0].entries[0].value, "");
}

TEST_CASE(reports_line_numbers_on_errors)
{
    try {
        static_cast<void>(pw::parse_ini("[s]\nok = 1\nbroken line\n"));
        CHECK(false);
    } catch (const pw::ConfigError& error) {
        CHECK_EQ(error.line(), 3);
    }
    CHECK_THROWS(pw::ConfigError, pw::parse_ini("key = outside\n"));
    CHECK_THROWS(pw::ConfigError, pw::parse_ini("[unterminated\n"));
    CHECK_THROWS(pw::ConfigError, pw::parse_ini("[]\n"));
    CHECK_THROWS(pw::ConfigError, pw::parse_ini("[s]\n= value\n"));
}

TEST_MAIN()
