# process-watchdog

**A small, dependency-free Linux process supervisor in modern C++17: restart policies with exponential backoff, heartbeat watchdogs, resource limits and rotating log capture — one INI file, two binaries.**

[![CI](https://github.com/bekirhankarakurt/process-watchdog/actions/workflows/ci.yml/badge.svg)](https://github.com/bekirhankarakurt/process-watchdog/actions/workflows/ci.yml)
[![CodeQL](https://github.com/bekirhankarakurt/process-watchdog/actions/workflows/codeql.yml/badge.svg)](https://github.com/bekirhankarakurt/process-watchdog/actions/workflows/codeql.yml)
[![Coverage](https://img.shields.io/endpoint?url=https://bekirhankarakurt.github.io/process-watchdog/coverage.json)](https://bekirhankarakurt.github.io/process-watchdog/coverage/)
[![Release](https://img.shields.io/github/v/release/bekirhankarakurt/process-watchdog)](https://github.com/bekirhankarakurt/process-watchdog/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey?logo=linux)

📖 [API documentation](https://bekirhankarakurt.github.io/process-watchdog/api/) · 📊 [Coverage report](https://bekirhankarakurt.github.io/process-watchdog/coverage/)

![demo](docs/demo.gif)

## Why

- **Hung is not the same as alive.** A process that is still in the process table but stuck in a deadlock is caught by heartbeat timeouts and restarted, not just crashes.
- **No crash storms.** Exponential backoff, a consecutive-restart limit and a "stable run" reset keep a broken service from burning CPU or flooding logs.
- **Predictable resource envelope.** `setrlimit` limits (CPU, memory, files, core, processes) are applied in the child before `exec`.
- **Small and auditable.** ~3.4k lines of C++17 (including headers), no third-party dependencies, single-threaded `epoll` + `signalfd` loop — suitable for embedded Linux targets and ground stations as well as dev machines.
- **Operable.** `pwctl` lists, starts, stops and restarts services over a UNIX control socket; output is captured into rotating log files.

## Quick start (60 seconds)

```bash
git clone https://github.com/bekirhankarakurt/process-watchdog.git && cd process-watchdog
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
export PATH="$PWD/build:$PATH"
pwatchd --check -c examples/watchdog.ini         # validate and summarise
pwatchd -c examples/watchdog.ini 2> pwatchd.log & # run in the background
pwctl list                                        # see services, restarts, heartbeats
pwctl start nightly-report && tail -n 3 examples/logs/flaky-worker.log
pwctl shutdown
```

Prebuilt `linux-x86_64` and `linux-aarch64` tarballs (with `SHA256SUMS`) are on the [releases page](https://github.com/bekirhankarakurt/process-watchdog/releases/latest).

## Requirements and build options

| Requirement | Version |
|---|---|
| Linux kernel | 2.6.27+ (`signalfd`, `epoll_create1`, `pipe2`) |
| Compiler | GCC 9+ or Clang 10+ with C++17 |
| CMake | 3.16+ |
| Runtime dependencies | none (glibc / libstdc++ only) |

| CMake option | Default | Effect |
|---|---|---|
| `PW_BUILD_TESTS` | `ON` | Build unit tests, integration tests and the `pw-crasher` helper |
| `PW_WARNINGS_AS_ERRORS` | `OFF` | Add `-Werror` (CI uses `ON`) |
| `PW_ENABLE_SANITIZERS` | `OFF` | Build with AddressSanitizer + UndefinedBehaviorSanitizer |
| `PW_ENABLE_COVERAGE` | `OFF` | Instrument with `--coverage` for gcov/gcovr |
| `CMAKE_BUILD_TYPE` | `RelWithDebInfo` | Standard CMake build type |

Install with `cmake --install build --prefix /usr/local`.

## Usage

### `pwatchd` — the supervisor

| Option | Description |
|---|---|
| `-c, --config FILE` | Configuration file (required). Relative paths inside it resolve against its directory. |
| `-n, --check` | Validate the configuration, print a service summary and exit (dry run). |
| `-l, --log-level LEVEL` | Override `log_level`: `debug`, `info`, `warning`, `error`. |
| `-V, --version` / `-h, --help` | Version / usage. |

`pwatchd` runs in the foreground and logs to stderr. `SIGTERM`/`SIGINT` (or `pwctl shutdown`) stop all services gracefully and exit 0.

### `pwctl` — the client

| Command | Description |
|---|---|
| `pwctl list` | Table of all services: state, pid, restarts, uptime, heartbeat age, last exit |
| `pwctl status NAME` | All fields of one service |
| `pwctl start NAME\|all` | Start a stopped/exited/failed service (resets the restart counter) |
| `pwctl stop NAME\|all` | Send `stop_signal`, escalate to `SIGKILL` after `stop_timeout` |
| `pwctl restart NAME\|all` | Stop and start again |
| `pwctl shutdown` | Stop everything and terminate `pwatchd` |
| `pwctl beat [NAME]` | Send one heartbeat (used *inside* a supervised service) |

Options: `-s PATH` control socket (default `$PW_CONTROL_SOCKET` or `/tmp/pwatchd.sock`), `-t DURATION` timeout, `--raw` protocol output. Exit codes: `0` ok, `1` rejected, `2` usage, `3` cannot connect.

### Configuration reference

```ini
[watchdog]
control_socket   = /run/pwatchd/ctl.sock   # default /tmp/pwatchd.sock
heartbeat_socket = /run/pwatchd/hb.sock    # UNIX datagram heartbeats (optional)
heartbeat_udp    = 127.0.0.1:9900          # UDP heartbeats (optional)
log_dir          = /var/log/pwatchd        # default ./logs next to the config
log_level        = info

[service:NAME]                             # NAME: [A-Za-z0-9_.-]{1,64}
command = /usr/bin/app --flag "quoted arg" # split like a shell, never run through one
```

| Service key | Default | Meaning |
|---|---|---|
| `command` | — | Program and arguments (`'…'`, `"…"`, `\` quoting; `PATH` lookup) |
| `working_dir` | inherited | Directory to `chdir` into |
| `env` | — | `KEY=VALUE`, repeatable; added to the inherited environment |
| `restart` | `on-failure` | `never`, `on-failure` (non-zero exit, signal, heartbeat timeout), `always` |
| `backoff_initial` / `backoff_max` / `backoff_multiplier` | `500ms` / `30s` / `2` | Delay before restart *n* = `initial × multiplier^n`, capped |
| `max_restarts` | `5` | Consecutive restarts before state `failed`; `unlimited` allowed |
| `stable_after` | `10s` | A run at least this long resets the counter and backoff |
| `heartbeat_timeout` | `0` (off) | Max silence between heartbeats before the process is stopped and treated as failed |
| `heartbeat_grace` | = timeout | Time allowed for the first heartbeat after start |
| `stop_signal` / `stop_timeout` | `TERM` / `5s` | Graceful stop signal, then `SIGKILL` |
| `autostart` | `true` | Start with `pwatchd`, or only on `pwctl start` |
| `log_file` | `<log_dir>/<name>.log` | Captured stdout + stderr |
| `log_max_size` / `log_max_files` | `10M` / `5` | Rotate to `.1 … .N`; `0` files truncates in place, `0` size disables rotation |
| `log_timestamps` | `true` | Prefix each captured line with local time |
| `limit_cpu` `limit_nofile` `limit_as` `limit_core` `limit_nproc` `limit_fsize` | unset | `setrlimit` soft = hard; sizes accept `K/M/G`, CPU accepts durations |

Durations: `250ms`, `5s`, `2m`, `1h` (bare number = seconds). Unknown keys are errors, so typos are caught by `--check`.

### Recipes

**1. Restart a crashing daemon, but give up on a crash loop**

```ini
[service:telemetry-bridge]
command         = /opt/gs/bin/telemetry-bridge --port 5600
restart         = on-failure
backoff_initial = 1s
backoff_max     = 1m
max_restarts    = 10
stable_after    = 2m
```

**2. Detect a hung process with heartbeats** — any language can send the datagram; from a shell use `pwctl beat`:

```bash
# inside the service: the environment provides PW_SERVICE, PW_HEARTBEAT_TOKEN, PW_HEARTBEAT_SOCKET
while process_next_frame; do pwctl beat; done
```

```python
# Python service: send the heartbeat directly over the UNIX datagram socket
import os, socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.sendto(f"HB {os.environ['PW_SERVICE']} {os.environ['PW_HEARTBEAT_TOKEN']}\n".encode(),
         os.environ["PW_HEARTBEAT_SOCKET"])
```

**3. Contain a memory-leaking or fd-leaking tool**

```ini
[service:image-pipeline]
command      = ./pipeline --input /data/frames
limit_as     = 1G
limit_nofile = 256
limit_core   = 0
log_max_size = 50M
log_max_files = 3
```

**4. Run pwatchd itself under systemd**

```ini
# /etc/systemd/system/pwatchd.service
[Service]
ExecStart=/usr/local/bin/pwatchd -c /etc/pwatchd/watchdog.ini
User=pwatchd
Restart=on-failure
KillMode=mixed
```

**5. Scripted health check / rolling restart**

```bash
pwctl --raw list | grep -q 'state=failed' && echo "ALERT: failed services"
for s in api worker; do pwctl restart "$s"; sleep 5; done
```

## Architecture

```
                 pwctl ──► control socket (UNIX stream, 0600) ──┐
 service ── pwctl beat / own code ──► heartbeat socket / UDP ───┤
                                                                ▼
 ┌──────────────────────────── pwatchd (single thread) ───────────────────────────┐
 │  epoll_wait(timeout = nearest deadline)                                        │
 │    ├─ signalfd: SIGCHLD → waitpid(WNOHANG) loop → on_exit → RestartTracker     │
 │    │            SIGTERM/SIGINT → graceful shutdown                             │
 │    ├─ control fd → list/status/start/stop/restart/shutdown                     │
 │    ├─ heartbeat fds → token check → HeartbeatMonitor.beat()                    │
 │    └─ child pipes (stdout+stderr) → RotatingLog                                │
 │  timers: backoff expiry → spawn · stop timeout → SIGKILL · heartbeat → stop    │
 └───────────────┬────────────────────────────────────────────────────────────────┘
                 │ fork → setpgid → sigmask reset → setrlimit → dup2 → chdir → execvpe
                 ▼             (exec errors reported through a CLOEXEC pipe)
           service process group

 State machine per service:
   stopped ──start──► running ──exit──► RestartTracker ─┬─► backoff ──delay──► running
      ▲                  │                              ├─► exited  (policy: no restart)
      └──stop── stopping ◄── stop / restart / heartbeat └─► failed  (limit reached)
```

| Module | Responsibility |
|---|---|
| `ini`, `config` | Strict INI parsing with line numbers, validation, path resolution |
| `restart` | Pure policy + backoff logic (unit-tested without processes) |
| `heartbeat` | Wire format, per-start tokens, deadline tracking, senders |
| `process` | `fork`/`execvpe` with rlimits, process groups, exit-status decoding |
| `rotating_log` | Size-based rotation at line boundaries, optional timestamps |
| `control`, `socket` | Line protocol, RAII socket helpers |
| `supervisor` | The `epoll`/`signalfd` event loop and state machine |

**Design trade-offs**

- *Single thread, no locks.* All events go through one `epoll` set; `signalfd` turns `SIGCHLD` into a readable fd, so there is no async-signal-handler state. The cost: a control client is served synchronously (bounded by a 1 s socket timeout).
- *Process groups.* Each child leads its own group and signals go to `-pid`, so shells and their sub-processes are stopped together. A child that calls `setsid()` escapes this — use `stop_signal` handling in the child.
- *Heartbeat tokens.* A fresh 64-bit token per start means a stale instance (or a stray script) cannot keep a new instance "alive". It is an integrity check, not authentication — keep UDP on loopback.
- *Logs are written by the supervisor,* not by the child, so rotation is safe and the child needs no logging code; the trade-off is one copy through a pipe.
- *No config reload.* `SIGHUP` is ignored deliberately; restart `pwatchd` for config changes (see roadmap).

## Protocols

**Control socket** (UNIX stream): client sends one line, server answers and closes.

| Request | Response |
|---|---|
| `list` | `OK N services` + one record per service |
| `status NAME` | `OK` + one record |
| `start\|stop\|restart NAME\|all` | `OK` + `name: started\|stopping\|restarting\|already running\|not running\|…` |
| `shutdown` | `OK shutting down` |
| anything invalid | `ERR <reason>` |

Record = tab-separated `key=value`: `name`, `state` (`stopped running stopping backoff exited failed`), `pid`, `restarts`, `uptime`, `heartbeat` (age of last beat, `none`, or `-` if disabled), `next_restart`, `policy`, `last_exit` (`exit 3`, `signal 11 (SIGSEGV)`, `… after heartbeat timeout`, spawn error), `log`.

**Heartbeat datagram** (UNIX datagram or UDP): `HB <service> <token>\n`, token = 16 hex digits.

**Child environment**

| Variable | Value |
|---|---|
| `PW_SERVICE` | Service name |
| `PW_HEARTBEAT_TOKEN` | Token for this process instance |
| `PW_HEARTBEAT_SOCKET` / `PW_HEARTBEAT_UDP` | Heartbeat endpoint(s), if configured |
| `PW_CONTROL_SOCKET` | Control socket path (lets `pwctl` work inside services) |

## Testing

```bash
ctest --test-dir build --output-on-failure -j        # 8 unit suites + 17 integration scenarios
ctest --test-dir build -L integration                # only end-to-end scenarios
./build/tests/integration_tests heartbeat_timeout_restarts build/pwatchd build/tests/pw-crasher build/pwctl
```

- **Unit tests** cover parsing, backoff math, heartbeat deadlines, log rotation, spawning (`exit`, signals, rlimits, `chdir`/`exec` failures) and the protocol.
- **Integration tests** start the real `pwatchd` against `pw-crasher`, a helper that deliberately exits, segfaults, ignores `SIGTERM`, stops heart-beating or floods output (`pw-crasher beat:3:50 hang`). Each scenario uses its own temp directory, so they run in parallel.
- **CI** builds GCC and Clang (Debug + Release, `-Werror`), runs an ASan/UBSan job, clang-tidy + cppcheck, CodeQL, and publishes coverage and API docs to GitHub Pages. No hardware or root is required.

## Troubleshooting / FAQ

| Symptom | Cause / fix |
|---|---|
| `another instance is already listening on …` | A `pwatchd` already owns that control socket; use a different `control_socket` or `pwctl shutdown` it. A stale file from a crash is removed automatically. |
| `pwctl: connect …: No such file or directory` (exit 3) | `pwatchd` not running or different socket: pass `-s` or set `PW_CONTROL_SOCKET`. |
| Service is `failed` with `exec '…': No such file or directory` | Wrong path or `PATH`; commands are not run through a shell — wrap with `/bin/sh -c '…'` if you need pipes or globs. |
| Restarted with `… after heartbeat timeout` although it works | First beat arrives too late: raise `heartbeat_grace`, or beat more often than `heartbeat_timeout`. |
| `last_exit` is `signal 9 (SIGKILL)` on every stop | The process ignores `stop_signal`; handle it or raise `stop_timeout`. |
| `UNIX socket path … longer than 107 bytes` | Linux limits `sun_path`; use a shorter path such as `/run/pwatchd/ctl.sock`. |
| `failed` after a burst of crashes, then fine later | Intended: `max_restarts` reached. Fix the cause and `pwctl start NAME`. |

## Project layout

```
process-watchdog/
├── apps/
│   ├── pwatchd.cpp          # supervisor daemon CLI
│   └── pwctl.cpp            # control / heartbeat client
├── include/pw/              # public headers (documented with Doxygen)
├── src/                     # library: config, restart, heartbeat, process, supervisor, …
├── tests/
│   ├── unit/                # one executable per module
│   ├── integration/         # end-to-end scenarios against the real daemon
│   ├── helpers/crasher.cpp  # deliberately misbehaving child program
│   └── test_support.hpp     # tiny dependency-free test harness
├── examples/watchdog.ini    # annotated example configuration
├── docs/                    # demo.tape (vhs), demo.ini, demo.gif
├── cmake/version.hpp.in     # generated version header
├── .github/workflows/       # CI, CodeQL, coverage & docs (Pages), release
├── .clang-tidy  Doxyfile  CHANGELOG.md  CONTRIBUTING.md  SECURITY.md
└── CMakeLists.txt
```

## Roadmap

- [x] Restart policies with exponential backoff and restart limit
- [x] UNIX-socket and UDP heartbeats with per-start tokens
- [x] Resource limits, rotating log capture, SIGKILL escalation
- [x] Control socket + `pwctl`, `--check` dry run
- [ ] Configuration reload on `SIGHUP` (add/remove/changed services)
- [ ] Service dependencies and start ordering
- [ ] cgroup v2 limits (memory.max, cpu.max) as an alternative to `setrlimit`
- [ ] Privilege dropping per service (`user`, `group`)
- [ ] Prometheus/JSON status export

## License

MIT — see [LICENSE](LICENSE).

Bekirhan Karakurt - [github.com/bekirhankarakurt](https://github.com/bekirhankarakurt)
