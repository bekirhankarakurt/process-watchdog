/// @file ini.hpp
/// @brief Strict, line-oriented INI parser.
#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace pw {

/// A `key = value` line inside a section. Duplicate keys are preserved in order.
struct IniEntry {
    std::string key;
    std::string value;
    int line{0};
};

/// A `[name]` section together with its entries.
struct IniSection {
    std::string name;
    int line{0};
    std::vector<IniEntry> entries;
};

/// Syntax or validation error with the offending 1-based line number (0 if none).
class ConfigError : public std::runtime_error {
public:
    ConfigError(int line, const std::string& message);
    [[nodiscard]] int line() const noexcept { return line_; }

private:
    int line_;
};

/// Parses INI text. Lines starting with `#` or `;` are comments; keys are
/// case-sensitive; values are trimmed but otherwise kept verbatim.
/// @throws ConfigError on malformed lines or entries outside a section.
[[nodiscard]] std::vector<IniSection> parse_ini(std::string_view text);

}  // namespace pw
