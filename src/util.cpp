#include "pw/util.hpp"

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace pw {
namespace {

[[nodiscard]] bool is_space(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

[[nodiscard]] bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

/// Splits `123abc` into the numeric prefix and the remaining suffix.
[[nodiscard]] std::pair<std::string_view, std::string_view> split_number(std::string_view text) noexcept
{
    std::size_t i = 0;
    while (i < text.size() && is_digit(text[i])) {
        ++i;
    }
    return {text.substr(0, i), trim(text.substr(i))};
}

[[nodiscard]] std::optional<std::uint64_t> checked_multiply(std::uint64_t value, std::uint64_t factor) noexcept
{
    if (factor != 0 && value > std::numeric_limits<std::uint64_t>::max() / factor) {
        return std::nullopt;
    }
    return value * factor;
}

}  // namespace

std::string_view trim(std::string_view text) noexcept
{
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

std::vector<std::string> split_command(std::string_view line)
{
    enum class Mode { Plain, Single, Double };
    std::vector<std::string> words;
    std::string current;
    bool in_word = false;
    Mode mode = Mode::Plain;

    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        switch (mode) {
        case Mode::Plain:
            if (is_space(c)) {
                if (in_word) {
                    words.push_back(std::move(current));
                    current.clear();
                    in_word = false;
                }
            } else if (c == '\'') {
                mode = Mode::Single;
                in_word = true;
            } else if (c == '"') {
                mode = Mode::Double;
                in_word = true;
            } else if (c == '\\') {
                if (i + 1 >= line.size()) {
                    throw std::invalid_argument("trailing backslash");
                }
                current.push_back(line[++i]);
                in_word = true;
            } else {
                current.push_back(c);
                in_word = true;
            }
            break;
        case Mode::Single:
            if (c == '\'') {
                mode = Mode::Plain;
            } else {
                current.push_back(c);
            }
            break;
        case Mode::Double:
            if (c == '"') {
                mode = Mode::Plain;
            } else if (c == '\\' && i + 1 < line.size() &&
                       (line[i + 1] == '"' || line[i + 1] == '\\' || line[i + 1] == '$')) {
                current.push_back(line[++i]);
            } else {
                current.push_back(c);
            }
            break;
        }
    }
    if (mode != Mode::Plain) {
        throw std::invalid_argument("unterminated quote");
    }
    if (in_word) {
        words.push_back(std::move(current));
    }
    return words;
}

std::optional<std::uint64_t> parse_uint(std::string_view text) noexcept
{
    if (text.empty() || !is_digit(text.front())) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const char* const end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> parse_double(std::string_view text) noexcept
{
    if (text.empty() || !(is_digit(text.front()) || text.front() == '.')) {
        return std::nullopt;
    }
    double value = 0.0;
    const char* const end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc{} || ptr != end || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<Millis> parse_duration(std::string_view text) noexcept
{
    const auto [digits, unit] = split_number(trim(text));
    const auto value = parse_uint(digits);
    if (!value) {
        return std::nullopt;
    }
    std::uint64_t factor = 0;
    if (unit == "ms") {
        factor = 1;
    } else if (unit.empty() || unit == "s") {
        factor = 1000;
    } else if (unit == "m" || unit == "min") {
        factor = 60ULL * 1000ULL;
    } else if (unit == "h") {
        factor = 3600ULL * 1000ULL;
    } else {
        return std::nullopt;
    }
    const auto ms = checked_multiply(*value, factor);
    if (!ms || *ms > static_cast<std::uint64_t>(std::numeric_limits<Millis::rep>::max())) {
        return std::nullopt;
    }
    return Millis{static_cast<Millis::rep>(*ms)};
}

std::optional<std::uint64_t> parse_size(std::string_view text) noexcept
{
    const auto [digits, unit] = split_number(trim(text));
    const auto value = parse_uint(digits);
    if (!value) {
        return std::nullopt;
    }
    const std::string lowered = to_lower(unit);
    std::uint64_t factor = 0;
    if (lowered.empty() || lowered == "b") {
        factor = 1;
    } else if (lowered == "k" || lowered == "kb" || lowered == "kib") {
        factor = 1024ULL;
    } else if (lowered == "m" || lowered == "mb" || lowered == "mib") {
        factor = 1024ULL * 1024ULL;
    } else if (lowered == "g" || lowered == "gb" || lowered == "gib") {
        factor = 1024ULL * 1024ULL * 1024ULL;
    } else {
        return std::nullopt;
    }
    return checked_multiply(*value, factor);
}

std::optional<bool> parse_bool(std::string_view text) noexcept
{
    const std::string lowered = to_lower(trim(text));
    if (lowered == "true" || lowered == "yes" || lowered == "on" || lowered == "1") {
        return true;
    }
    if (lowered == "false" || lowered == "no" || lowered == "off" || lowered == "0") {
        return false;
    }
    return std::nullopt;
}

std::string format_duration(Millis duration)
{
    const auto ms = duration.count() < 0 ? 0 : duration.count();
    std::array<char, 32> buffer{};
    constexpr Millis::rep second = 1000;
    constexpr Millis::rep minute = 60 * second;
    constexpr Millis::rep hour = 60 * minute;
    constexpr Millis::rep day = 24 * hour;
    int written = 0;
    if (ms < second) {
        written = std::snprintf(buffer.data(), buffer.size(), "%lldms", static_cast<long long>(ms));
    } else if (ms < minute) {
        written = std::snprintf(buffer.data(), buffer.size(), "%lld.%llds", static_cast<long long>(ms / second),
                                static_cast<long long>((ms % second) / 100));
    } else if (ms < hour) {
        written = std::snprintf(buffer.data(), buffer.size(), "%lldm%02llds", static_cast<long long>(ms / minute),
                                static_cast<long long>((ms % minute) / second));
    } else if (ms < day) {
        written = std::snprintf(buffer.data(), buffer.size(), "%lldh%02lldm", static_cast<long long>(ms / hour),
                                static_cast<long long>((ms % hour) / minute));
    } else {
        written = std::snprintf(buffer.data(), buffer.size(), "%lldd%02lldh", static_cast<long long>(ms / day),
                                static_cast<long long>((ms % day) / hour));
    }
    return {buffer.data(), written > 0 ? static_cast<std::size_t>(written) : 0U};
}

std::string to_lower(std::string_view text)
{
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace pw
