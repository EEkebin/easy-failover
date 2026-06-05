# easy-failover

[![CI](https://github.com/EEkebin/easy-failover/actions/workflows/ci.yml/badge.svg)](https://github.com/EEkebin/easy-failover/actions/workflows/ci.yml)

easy-failover is a lightweight C++ Linux agent for automatic virtual IP failover between nodes.

This repository is named `easy-failover`, and the binary is named `easy-failover`.
The project is licensed under Apache-2.0.

## Contents

- [Status](#status)
- [Platform Targets](#platform-targets)
- [Build](#build)
- [Install](#install)
- [Run](#run)
- [Runtime Logging](#runtime-logging)
- [Configuration](#configuration)
- [Automation](#automation)
- [Contribution Workflow](#contribution-workflow)
- [Optional Dashboard](#optional-dashboard)

## Status

early WIP. The current codebase builds and tests a Linux VIP failover agent with CLI parsing,
logging, TOML config loading and validation, heartbeat transport integration, guarded VIP
operations, a runtime decision loop, and a disabled-by-default read-only local API. Real VIP
movement is still opt-in and should be validated in a lab before production use.

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

## Install

easy-failover does not publish distro packages yet. Install from source with CMake or stage the
install tree and package it yourself. The install rules place the binary, example config, systemd
unit, README, license, and docs using the layout described in
[`docs/install-package-layout.md`](docs/install-package-layout.md).

Install build dependencies with your distro package manager. Package names vary, but a typical
Linux build host needs:

- a C++23-capable compiler such as GCC or Clang;
- CMake;
- `make` or Ninja;
- Git;
- `iproute2` and `arping` for hosts that will test real VIP movement.

For a production-style source install under `/usr`, with configuration under `/etc`, use:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_SYSCONFDIR=/etc
cmake --build build
sudo cmake --install build --prefix /usr
```

The install step stages an example config, not an active config:

```text
/etc/easy-failover/config.example.toml
```

Create the active config yourself and edit it for the node before starting the daemon:

```sh
sudo install -d -m 0755 /etc/easy-failover
sudo cp /etc/easy-failover/config.example.toml /etc/easy-failover/config.toml
sudo editor /etc/easy-failover/config.toml
easy-failover --config /etc/easy-failover/config.toml --validate-config
```

For a local `/usr/local` install instead, leave `CMAKE_INSTALL_SYSCONFDIR` at its default:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build --prefix /usr/local
```

That installs the example config under the local prefix:

```text
/usr/local/etc/easy-failover/config.example.toml
```

Use the same prefix when creating and validating the active config:

```sh
sudo install -d -m 0755 /usr/local/etc/easy-failover
sudo cp /usr/local/etc/easy-failover/config.example.toml /usr/local/etc/easy-failover/config.toml
sudo editor /usr/local/etc/easy-failover/config.toml
easy-failover --config /usr/local/etc/easy-failover/config.toml --validate-config
```

If the systemd unit was installed, reload systemd and enable/start only after the config is valid:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now easy-failover.service
sudo systemctl status easy-failover.service
```

For a source checkout without installing, keep using the build-tree binary:

```sh
./build/easy-failover --config configs/easy-failover.toml --dry-run
```

Uninstalling a manual CMake install is distro- and prefix-dependent. For package builds, prefer the
package manager. For manual source installs, inspect the staged layout first with:

```sh
cmake --install build --prefix "$PWD/stage"
find "$PWD/stage" -type f | sort
```

Packaging guidance lives in [`docs/distro-packaging-notes.md`](docs/distro-packaging-notes.md).
Do not enable real VIP mutation until the node config, Linux capabilities, and failover behavior
have been tested in your target environment.

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
capability notes live in [`docs/linux-capabilities.md`](docs/linux-capabilities.md). The local
read-only API is disabled by default and documented in
[`docs/local-api-design.md`](docs/local-api-design.md).
Install/package layout notes live in
[`docs/install-package-layout.md`](docs/install-package-layout.md). Distro packaging guidance lives
in [`docs/distro-packaging-notes.md`](docs/distro-packaging-notes.md).
Real VIP movement is guarded by runtime dry-run mode and
`mutation_safety.allow_network_mutation`. Quorum/fencing behavior remains future work, so validate
carefully before using real network mutation.

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

## Optional Dashboard

An optional read-only Next.js dashboard lives under `web/`. It is not built or installed by CMake
and remains separate from the daemon binary.

```sh
cd web
npm install
npm run dev
```

The dashboard reads the local HTTP API when the daemon is running with `api.enabled = true` and
falls back to sample data otherwise. It does not expose VIP mutation, daemon controls, config
writes, or other privileged actions.
