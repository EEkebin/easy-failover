# easy-failover

[![CI](https://github.com/EEkebin/easy-failover/actions/workflows/ci.yml/badge.svg)](https://github.com/EEkebin/easy-failover/actions/workflows/ci.yml)

easy-failover is a lightweight C++ Linux agent for automatic virtual IP failover between nodes.

This repository is named `easy-failover`, and the binary is named `easy-failover`.
The project is licensed under Apache-2.0.

## Status

early WIP. The current codebase is a compiling project skeleton with CLI parsing, logging,
TOML config parsing, config validation, basic node state types, a simple election helper, and
non-mutating Linux VIP command construction. It does not move VIPs, run heartbeat networking, or
run a real daemon loop yet.

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
ctest --test-dir build --output-on-failure
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

## Configuration

The sample config lives at `configs/easy-failover.toml`. A minimal config only needs the VIP and
peer/server pool. See [`docs/config-reference.md`](docs/config-reference.md) for the current TOML
schema, defaults, and validation rules.

Failover safety notes live in [`docs/failover-safety.md`](docs/failover-safety.md). Real VIP
movement is intentionally blocked until heartbeat, health, quorum/split-brain, and explicit
operator-safety controls are designed and implemented.

Example production config path:

```text
/etc/easy-failover/config.toml
```

Example systemd service name:

```text
easy-failover.service
```

## Automation

GitHub Actions runs CI on pull requests and pushes to `main`. The current CI job builds the
project on Linux, runs CTest, and runs smoke checks for `--version`, config validation, and dry-run
mode.

Release automation runs on tags matching `v*` and can also be started manually. Tagged releases
build a Linux x86_64 tarball and publish it to the GitHub release.

## Contribution Workflow

The `main` branch is protected. Changes should be made on a feature branch and merged through a
pull request after the required CI check passes.

Expected workflow:

```sh
git checkout -b feature/my-change
git add .
git commit -m "Describe your change"
git push -u origin feature/my-change
```

Pull requests must pass the `Linux smoke build` status check before merging. The repository uses
squash merging to keep `main` linear and readable.

## Future Dashboard

A future optional Next.js dashboard may be added under `web/` in this same repository. It is not
implemented yet.
