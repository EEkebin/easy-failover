# easyFailover

easyFailover is a lightweight C++ Linux agent for automatic virtual IP failover between nodes.

This repository is named `easy-failover`, and the binary is named `easy-failover`.
The project is licensed under Apache-2.0.

## Status

early WIP. The current codebase is a compiling project skeleton with CLI parsing, logging,
TOML config parsing, config validation, basic node state types, a simple election helper, and
Linux VIP manager stubs. It does not move VIPs, run heartbeat networking, or run a real daemon
loop yet.

## Platform Targets

Primary Linux targets:

- Arch Linux latest
- Ubuntu latest LTS and latest stable
- Debian latest stable
- Fedora latest stable
- RHEL latest
- Rocky Linux latest

Future planned targets:

- Windows 11
- Windows Server 2025
- latest macOS

## Build

```sh
cmake -S . -B build
cmake --build build
```

## Run

Validate the sample config:

```sh
./build/easy-failover --config configs/easy-failover.toml --validate-config
```

Run in dry-run mode:

```sh
./build/easy-failover --config configs/easy-failover.toml --dry-run
```

Example production config path:

```text
/etc/easy-failover/config.toml
```

Example systemd service name:

```text
easy-failover.service
```

## Future Dashboard

A future optional Next.js dashboard may be added under `web/` in this same repository. It is not
implemented yet.
