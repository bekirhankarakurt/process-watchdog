// Minimal dependency-free test harness.
#pragma once

#include <cstdio>
#include <exception>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pwtest {

struct TestCase {
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

class Failure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline void check(bool condition, const char* expression, const char* file, int line)
{
    if (!condition) {
        std::ostringstream message;
        message << file << ":" << line << ": CHECK(" << expression << ") failed";
        throw Failure(message.str());
    }
}

template <typename A, typename B>
void check_eq(const A& actual, const B& expected, const char* actual_text, const char* expected_text,
              const char* file, int line)
{
    if (!(actual == expected)) {
        std::ostringstream message;
        message << file << ":" << line << ": CHECK_EQ(" << actual_text << ", " << expected_text << ") failed: '"
                << actual << "' != '" << expected << "'";
        throw Failure(message.str());
    }
}

template <typename Exception, typename Body>
void check_throws(Body&& body, const char* expression, const char* file, int line)
{
    try {
        std::forward<Body>(body)();
    } catch (const Exception&) {
        return;
    }
    std::ostringstream message;
    message << file << ":" << line << ": CHECK_THROWS(" << expression << ") did not throw";
    throw Failure(message.str());
}

/// Runs all registered tests (or only the one named in argv[1]).
inline int run_all(int argc, char** argv)
{
    const std::string filter = argc > 1 ? argv[1] : "";
    int failed = 0;
    int ran = 0;
    for (const TestCase& test : registry()) {
        if (!filter.empty() && test.name != filter) {
            continue;
        }
        ++ran;
        try {
            test.body();
            std::printf("[  OK  ] %s\n", test.name.c_str());
        } catch (const std::exception& error) {
            ++failed;
            std::printf("[ FAIL ] %s\n         %s\n", test.name.c_str(), error.what());
        }
    }
    std::printf("%d/%d passed\n", ran - failed, ran);
    return failed == 0 && ran > 0 ? 0 : 1;
}

}  // namespace pwtest

#define PW_CONCAT_INNER(a, b) a##b
#define PW_CONCAT(a, b) PW_CONCAT_INNER(a, b)
#define TEST_CASE(name)                                                                    \
    static void name();                                                                    \
    static const ::pwtest::Registrar PW_CONCAT(registrar_, name){#name, &name};            \
    static void name()
#define CHECK(expr) ::pwtest::check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(actual, expected) ::pwtest::check_eq((actual), (expected), #actual, #expected, __FILE__, __LINE__)
#define CHECK_THROWS(exception, expr) \
    ::pwtest::check_throws<exception>([&]() { static_cast<void>(expr); }, #expr, __FILE__, __LINE__)
#define TEST_MAIN() \
    int main(int argc, char** argv) { return ::pwtest::run_all(argc, argv); }
