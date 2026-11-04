/// @file process.hpp
/// @brief fork/exec process spawning with resource limits and exit status decoding.
#pragma once

#include "pw/config.hpp"

#include <sys/types.h>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace pw {

/// Everything needed to start a child process.
struct SpawnSpec {
    std::vector<std::string> argv;
    /// Complete environment as `KEY=VALUE` strings.
    std::vector<std::string> env;
    std::optional<std::filesystem::path> working_dir;
    std::vector<ResourceLimit> limits;
    /// Descriptor that becomes the child's stdout and stderr; -1 inherits.
    int output_fd{-1};
};

/// Raised when the child could not be set up or exec() failed.
class SpawnError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Forks and execs @p spec.argv (PATH lookup applies). The child becomes the
/// leader of a new process group, gets stdin from /dev/null and a clean
/// signal mask. Exec failures are reported synchronously through a CLOEXEC pipe.
/// @return the child's pid.
/// @throws SpawnError if fork, setup or exec fails.
[[nodiscard]] pid_t spawn_process(const SpawnSpec& spec);

/// Current environment with @p overrides (`KEY=VALUE`) replacing or appending.
[[nodiscard]] std::vector<std::string> build_environment(const std::vector<std::string>& overrides);

/// Maps a LimitKind to its RLIMIT_* constant.
[[nodiscard]] int to_rlimit_resource(LimitKind kind) noexcept;

/// How a process ended, decoded from a waitpid() status.
struct ExitStatus {
    enum class Kind { Exited, Signaled };
    Kind kind{Kind::Exited};
    int value{0};

    [[nodiscard]] static ExitStatus from_wait_status(int status) noexcept;
    /// True only for a normal exit with code 0.
    [[nodiscard]] bool success() const noexcept { return kind == Kind::Exited && value == 0; }
    /// `exit 3` or `signal 11 (SIGSEGV)`.
    [[nodiscard]] std::string describe() const;
};

}  // namespace pw
