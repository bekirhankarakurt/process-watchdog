#include "pw/fd.hpp"
#include "pw/process.hpp"
#include "test_support.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <csignal>
#include <string>

namespace {

int wait_for(pid_t pid)
{
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
    }
    return status;
}

/// Runs a shell snippet and returns its exit status and captured output.
std::pair<pw::ExitStatus, std::string> run_shell(const std::string& script, pw::SpawnSpec spec = {})
{
    std::array<int, 2> fds{};
    CHECK_EQ(::pipe2(fds.data(), O_CLOEXEC), 0);
    pw::UniqueFd read_end{fds[0]};
    pw::UniqueFd write_end{fds[1]};
    spec.argv = {"/bin/sh", "-c", script};
    if (spec.env.empty()) {
        spec.env = pw::build_environment({});
    }
    spec.output_fd = write_end.get();
    const pid_t pid = pw::spawn_process(spec);
    write_end.reset();
    std::string output;
    std::array<char, 256> buffer{};
    ssize_t count = 0;
    while ((count = ::read(read_end.get(), buffer.data(), buffer.size())) > 0) {
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return {pw::ExitStatus::from_wait_status(wait_for(pid)), output};
}

}  // namespace

TEST_CASE(exit_codes_and_output_are_captured)
{
    const auto [status, output] = run_shell("echo out; echo err >&2; exit 3");
    CHECK(status.kind == pw::ExitStatus::Kind::Exited);
    CHECK_EQ(status.value, 3);
    CHECK(!status.success());
    CHECK_EQ(status.describe(), "exit 3");
    CHECK_EQ(output, "out\nerr\n");
}

TEST_CASE(signals_are_decoded)
{
    const auto [status, output] = run_shell("kill -SEGV $$");
    CHECK(status.kind == pw::ExitStatus::Kind::Signaled);
    CHECK_EQ(status.value, SIGSEGV);
    CHECK_EQ(status.describe(), "signal 11 (SIGSEGV)");
}

TEST_CASE(child_runs_in_own_process_group_with_null_stdin)
{
    const auto [status, output] = run_shell("[ \"$(cut -d' ' -f5 /proc/$$/stat)\" = \"$$\" ] && cat && echo ok");
    CHECK(status.success());
    CHECK_EQ(output, "ok\n");
}

TEST_CASE(environment_and_working_directory_are_applied)
{
    pw::SpawnSpec spec;
    spec.env = pw::build_environment({"PW_TEST_VALUE=42", "PATH=/usr/bin:/bin"});
    spec.working_dir = "/tmp";
    const auto [status, output] = run_shell("echo $PW_TEST_VALUE $(pwd)", spec);
    CHECK(status.success());
    CHECK_EQ(output, "42 /tmp\n");
}

TEST_CASE(resource_limits_are_applied)
{
    pw::SpawnSpec spec;
    spec.limits = {{pw::LimitKind::OpenFiles, 77}, {pw::LimitKind::CoreSize, 0}};
    const auto [status, output] = run_shell("echo $(ulimit -n) $(ulimit -c)", spec);
    CHECK(status.success());
    CHECK_EQ(output, "77 0\n");
}

TEST_CASE(exec_and_chdir_failures_are_reported)
{
    pw::SpawnSpec missing;
    missing.argv = {"/nonexistent/binary"};
    missing.env = pw::build_environment({});
    CHECK_THROWS(pw::SpawnError, pw::spawn_process(missing));

    pw::SpawnSpec bad_dir;
    bad_dir.argv = {"/bin/true"};
    bad_dir.working_dir = "/nonexistent/dir";
    try {
        static_cast<void>(pw::spawn_process(bad_dir));
        CHECK(false);
    } catch (const pw::SpawnError& error) {
        CHECK(std::string(error.what()).find("chdir") != std::string::npos);
    }
    CHECK_THROWS(pw::SpawnError, pw::spawn_process(pw::SpawnSpec{}));
}

TEST_CASE(build_environment_overrides_existing_keys)
{
    ::setenv("PW_OVERRIDE_ME", "old", 1);
    const auto env = pw::build_environment({"PW_OVERRIDE_ME=new", "PW_FRESH=1"});
    int matches = 0;
    for (const std::string& item : env) {
        if (item.rfind("PW_OVERRIDE_ME=", 0) == 0) {
            ++matches;
            CHECK_EQ(item, "PW_OVERRIDE_ME=new");
        }
    }
    CHECK_EQ(matches, 1);
    CHECK_EQ(env.back(), "PW_FRESH=1");
}

TEST_CASE(unique_fd_moves_ownership)
{
    std::array<int, 2> fds{};
    CHECK_EQ(::pipe(fds.data()), 0);
    pw::UniqueFd a{fds[0]};
    pw::UniqueFd b{fds[1]};
    pw::UniqueFd moved = std::move(a);
    CHECK(!a.valid());  // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    CHECK_EQ(moved.get(), fds[0]);
    moved = std::move(b);
    CHECK_EQ(moved.get(), fds[1]);
    CHECK_EQ(::fcntl(fds[0], F_GETFD), -1);
}

TEST_MAIN()
