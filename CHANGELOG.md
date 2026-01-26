# Changelog

All notable changes to this project are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.0.0] - 2026-11-04

### Added
- `pwatchd` supervisor: fork/exec spawning into a dedicated process group, SIGCHLD
  handling via `signalfd`, single-threaded `epoll` event loop.
- Restart policies `never`, `on-failure` and `always` with exponential backoff,
  consecutive restart limit and `stable_after` reset.
- Heartbeat supervision over a UNIX datagram socket or UDP with per-start tokens,
  start-up grace period and timeout-triggered restarts.
- Resource limits (`limit_cpu`, `limit_nofile`, `limit_as`, `limit_core`,
  `limit_nproc`, `limit_fsize`) applied with `setrlimit` before `exec`.
- Stdout/stderr capture into size-rotated log files with optional timestamps.
- Graceful stop with configurable signal and SIGKILL escalation.
- `pwctl` client: `list`, `status`, `start`, `stop`, `restart`, `shutdown`, `beat`.
- `pwatchd --check` configuration validation with a service summary.
- Unit tests and integration tests driven by a deliberately crashing helper program.

[Unreleased]: https://github.com/bekirhankarakurt/process-watchdog/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/bekirhankarakurt/process-watchdog/releases/tag/v1.0.0
