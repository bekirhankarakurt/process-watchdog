#include "pw/ini.hpp"

#include "pw/util.hpp"

namespace pw {

ConfigError::ConfigError(int line, const std::string& message)
    : std::runtime_error(line > 0 ? "line " + std::to_string(line) + ": " + message : message), line_(line)
{
}

std::vector<IniSection> parse_ini(std::string_view text)
{
    std::vector<IniSection> sections;
    int line_number = 0;
    while (!text.empty()) {
        ++line_number;
        const std::size_t newline = text.find('\n');
        const std::string_view raw = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);

        const std::string_view line = trim(raw);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        if (line.front() == '[') {
            if (line.back() != ']') {
                throw ConfigError(line_number, "unterminated section header");
            }
            const std::string_view name = trim(line.substr(1, line.size() - 2));
            if (name.empty()) {
                throw ConfigError(line_number, "empty section name");
            }
            sections.push_back(IniSection{std::string(name), line_number, {}});
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            throw ConfigError(line_number, "expected 'key = value'");
        }
        const std::string_view key = trim(line.substr(0, equals));
        if (key.empty()) {
            throw ConfigError(line_number, "missing key before '='");
        }
        if (sections.empty()) {
            throw ConfigError(line_number, "entry outside of a section");
        }
        sections.back().entries.push_back(
            IniEntry{std::string(key), std::string(trim(line.substr(equals + 1))), line_number});
    }
    return sections;
}

}  // namespace pw
