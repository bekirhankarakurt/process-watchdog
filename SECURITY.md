# Security Policy

## Supported versions

| Version | Supported |
|---------|-----------|
| 1.0.x   | Yes       |
| < 1.0   | No        |

## Reporting a vulnerability

Please do **not** open a public issue for security problems. Use GitHub's
[private vulnerability reporting](https://github.com/bekirhankarakurt/process-watchdog/security/advisories/new)
instead. Include the version, configuration and steps to reproduce.
You can expect an acknowledgement within 7 days.

## Security model

- The control socket is created with mode `0600`; anyone who can connect can
  start and stop services, so keep it in a directory only the operator owns.
- Heartbeats carry a random 64-bit token issued per process start, so stale
  instances and other local users cannot keep a hung service alive by accident.
  The token is not a cryptographic authenticator: bind `heartbeat_udp` to
  loopback only.
- `pwatchd` never invokes a shell; commands are split and passed to `execvpe`.
  Run `pwatchd` as an unprivileged user; it does not drop privileges itself.
