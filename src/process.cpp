#include "pw/process.hpp"

#include "pw/fd.hpp"

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>

extern char** environ;  // NOLINT(readability-redundant-declaration,cppcoreguidelines-avoid-non-const-global-variables)

namespace pw {
namespace {

/// Setup step that failed in the child; sent to the parent with errno.
enum class ChildStage : int { Rlimit = 1, Stdio, Chdir, Exec };

struct ChildReport {
    ChildStage stage;
    int error;
};

[[nodiscard]] const char* stage_name(ChildStage stage) noexcept
{
    switch (stage) {
    case ChildStage::Rlimit:
        return "setrlimit";
    case ChildStage::Stdio:
        return "redirect stdio";
    case ChildStage::Chdir:
        return "chdir";
    case ChildStage::Exec:
        return "exec";
    }
    return "setup";
}

/// Reports a failed step to the parent and terminates. Async-signal-safe.
[[noreturn]] void child_fail(int report_fd, ChildStage stage) noexcept
{
    const ChildReport report{stage, errno};
    static_cast<void>(::write(report_fd, &report, sizeof(report)));
    ::_exit(127);
}

struct PreparedLimit {
    int resource;
    rlimit value;
};

/// Runs in the forked child; only async-signal-safe calls are allowed here.
[[noreturn]] void run_child(char* const* argv, char* const* envp, const char* working_dir,
                            const std::vector<PreparedLimit>& limits, int output_fd, int report_fd) noexcept
{
    static_cast<void>(::setpgid(0, 0));

    sigset_t empty;
    sigemptyset(&empty);
    static_cast<void>(::sigprocmask(SIG_SETMASK, &empty, nullptr));
    struct sigaction default_action {};
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    static_cast<void>(::sigaction(SIGPIPE, &default_action, nullptr));

    for (const PreparedLimit& limit : limits) {
        if (::setrlimit(limit.resource, &limit.value) != 0) {
            child_fail(report_fd, ChildStage::Rlimit);
        }
    }

    const int null_fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (null_fd < 0) {
        child_fail(report_fd, ChildStage::Stdio);
    }
    const int stdin_ok = null_fd == STDIN_FILENO ? ::fcntl(null_fd, F_SETFD, 0) : ::dup2(null_fd, STDIN_FILENO);
    if (stdin_ok < 0) {
        child_fail(report_fd, ChildStage::Stdio);
    }
    if (output_fd >= 0) {
        if (::dup2(output_fd, STDOUT_FILENO) < 0 || ::dup2(output_fd, STDERR_FILENO) < 0) {
            child_fail(report_fd, ChildStage::Stdio);
        }
    }

    if (working_dir != nullptr && ::chdir(working_dir) != 0) {
        child_fail(report_fd, ChildStage::Chdir);
    }

    ::execvpe(argv[0], argv, envp);
    child_fail(report_fd, ChildStage::Exec);
}

[[nodiscard]] std::vector<char*> pointer_array(std::vector<std::string>& strings)
{
    std::vector<char*> pointers;
    pointers.reserve(strings.size() + 1);
    for (std::string& item : strings) {  // cppcheck-suppress constVariableReference ; data() must be mutable
        pointers.push_back(item.data());
    }
    pointers.push_back(nullptr);
    return pointers;
}

}  // namespace

int to_rlimit_resource(LimitKind kind) noexcept
{
    switch (kind) {
    case LimitKind::CpuSeconds:
        return RLIMIT_CPU;
    case LimitKind::OpenFiles:
        return RLIMIT_NOFILE;
    case LimitKind::AddressSpace:
        return RLIMIT_AS;
    case LimitKind::CoreSize:
        return RLIMIT_CORE;
    case LimitKind::Processes:
        return RLIMIT_NPROC;
    case LimitKind::FileSize:
        return RLIMIT_FSIZE;
    }
    return RLIMIT_NOFILE;
}

std::vector<std::string> build_environment(const std::vector<std::string>& overrides)
{
    std::vector<std::string> env;
    for (char** item = environ; item != nullptr && *item != nullptr; ++item) {
        env.emplace_back(*item);
    }
    for (const std::string& entry : overrides) {
        const std::string key = entry.substr(0, entry.find('=') + 1);
        const auto existing = std::find_if(env.begin(), env.end(),
                                           [&](const std::string& item) { return item.rfind(key, 0) == 0; });
        if (existing != env.end()) {
            *existing = entry;
        } else {
            env.push_back(entry);
        }
    }
    return env;
}

pid_t spawn_process(const SpawnSpec& spec)
{
    if (spec.argv.empty()) {
        throw SpawnError("empty command");
    }
    // Everything the child needs is allocated before fork().
    std::vector<std::string> argv = spec.argv;
    std::vector<std::string> env = spec.env;
    std::vector<char*> argv_ptrs = pointer_array(argv);
    std::vector<char*> env_ptrs = pointer_array(env);
    const std::string working_dir = spec.working_dir ? spec.working_dir->string() : std::string{};
    std::vector<PreparedLimit> limits;
    for (const ResourceLimit& limit : spec.limits) {
        const auto value = static_cast<rlim_t>(limit.value);
        limits.push_back(PreparedLimit{to_rlimit_resource(limit.kind), rlimit{value, value}});
    }

    std::array<int, 2> report_pipe{};
    if (::pipe2(report_pipe.data(), O_CLOEXEC) != 0) {
        throw SpawnError(std::string("pipe2: ") + std::strerror(errno));
    }
    UniqueFd report_read{report_pipe[0]};
    UniqueFd report_write{report_pipe[1]};

    const pid_t pid = ::fork();
    if (pid < 0) {
        throw SpawnError(std::string("fork: ") + std::strerror(errno));
    }
    if (pid == 0) {
        run_child(argv_ptrs.data(), env_ptrs.data(), spec.working_dir ? working_dir.c_str() : nullptr, limits,
                  spec.output_fd, report_write.get());
    }
    // Also set the group from the parent so kill(-pid) works immediately.
    static_cast<void>(::setpgid(pid, pid));
    report_write.reset();

    ChildReport report{};
    ssize_t received = 0;
    do {
        received = ::read(report_read.get(), &report, sizeof(report));
    } while (received < 0 && errno == EINTR);

    if (received == static_cast<ssize_t>(sizeof(report))) {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        throw SpawnError(std::string(stage_name(report.stage)) + " '" + spec.argv.front() +
                         "': " + std::strerror(report.error));
    }
    return pid;
}

ExitStatus ExitStatus::from_wait_status(int status) noexcept
{
    if (WIFSIGNALED(status)) {
        return {Kind::Signaled, WTERMSIG(status)};
    }
    return {Kind::Exited, WEXITSTATUS(status)};
}

std::string ExitStatus::describe() const
{
    if (kind == Kind::Signaled) {
        return "signal " + std::to_string(value) + " (" + signal_name(value) + ")";
    }
    return "exit " + std::to_string(value);
}

}  // namespace pw
