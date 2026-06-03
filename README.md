# easy-failover

[![CI](https://github.com/EEkebin/easy-failover/actions/workflows/ci.yml/badge.svg)](https://github.com/EEkebin/easy-failover/actions/workflows/ci.yml)

easy-failover is a lightweight C++ Linux agent for automatic virtual IP failover between nodes.

This repository is named `easy-failover`, and the binary is named `easy-failover`.
The project is licensed under Apache-2.0.

## Status

early WIP. The current codebase is a compiling project skeleton with CLI parsing, logging,
TOML config parsing, config validation, basic node state types, a simple election helper, and
non-mutating Linux VIP command construction. It includes a one-iteration daemon lifecycle skeleton,
and minimal SIGINT/SIGTERM shutdown observation, but it does not move VIPs, run heartbeat
networking, or run a real daemon loop yet.

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

Stage the install layout without touching host paths:

```sh
cmake --install build --prefix "$PWD/stage"
```

For package-style builds that configure absolute install directories, stage through `DESTDIR`:

```sh
cmake -S . -B build -DCMAKE_INSTALL_SYSCONFDIR=/etc
DESTDIR="$PWD/stage-root" cmake --install build --prefix /usr
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

## Runtime Logging

The normal runtime path emits stable logfmt-style runtime events. Current event names are
`daemon_lifecycle_result` and `vip_operation`. Fields include lifecycle state, dry-run status,
validation error count, VIP operation count, VIP address/interface, operation type, success, and
error detail.

## Configuration

The sample config lives at `configs/easy-failover.toml`. A minimal config only needs the VIP and
peer/server pool. See [`docs/config-reference.md`](docs/config-reference.md) for the current TOML
schema, defaults, and validation rules.

Failover safety notes live in [`docs/failover-safety.md`](docs/failover-safety.md). Linux
capability notes live in [`docs/linux-capabilities.md`](docs/linux-capabilities.md). The local API
skeleton evaluates disabled/read-only startup state only; the future read-only API shape is
documented in [`docs/local-api-design.md`](docs/local-api-design.md).
Install/package layout notes live in
[`docs/install-package-layout.md`](docs/install-package-layout.md).
Real VIP movement is intentionally blocked until heartbeat, health, quorum/split-brain, and
explicit operator-safety controls are designed and implemented.

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
build a Linux x86_64 tarball, generate a SHA-256 checksum file, and publish both files to the
GitHub release.

Verify a downloaded release artifact:

```sh
sha256sum -c easy-failover-linux-x86_64.tar.gz.sha256
```

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
