# Contributing

Thanks for helping improve process-watchdog.

## Development setup

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPW_WARNINGS_AS_ERRORS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Before opening a pull request, also run the sanitizer build and static analysis:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DPW_ENABLE_SANITIZERS=ON
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure
run-clang-tidy -p build -quiet 'src/|apps/'
```

## Guidelines

- C++17, RAII for every resource (`UniqueFd`, smart pointers), no raw `new`/`delete`.
- Keep the build warning-free with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`.
- Policy logic stays pure (`restart.cpp`, `HeartbeatMonitor`) so it can be unit-tested
  without processes; system behaviour is covered by integration tests using `pw-crasher`.
- One logical change per commit, imperative commit messages ("Add UDP heartbeat test").
- Update `CHANGELOG.md` under `[Unreleased]` and the README when behaviour changes.
