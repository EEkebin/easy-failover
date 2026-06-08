# easy-failover

[![CI](https://github.com/EEkebin/easy-failover/actions/workflows/ci.yml/badge.svg)](https://github.com/EEkebin/easy-failover/actions/workflows/ci.yml)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![Language: C++23](https://img.shields.io/badge/C%2B%2B-23-00599C.svg)](#)

**Keep your service address alive when a server dies.** easy-failover is a lightweight Linux
daemon that moves a virtual IP (VIP) between nodes automatically, so the address your clients
talk to stays reachable even when the node behind it goes down.

---

## Contents

- [Introduction](#introduction)
- [Description](#description)
- [Installation](#installation)
- [Uninstallation](#uninstallation)
- [Contributing](#contributing)
- [Credits](#credits)
- [License](#license)
- [Disclaimer](#disclaimer)

## Introduction

When a server owning a shared service IP fails, that address goes dark until someone intervenes.
easy-failover removes the human from that loop: peer nodes exchange heartbeats, elect an owner,
and relocate the VIP to a healthy node — usually before anyone notices.

It's a single small binary, configured with one TOML file, with no external control plane,
no agents-of-agents, and no cloud dependency. Drop it on two or more Linux boxes and they sort
out the VIP between themselves.

## Description

easy-failover runs as a daemon on each node in a pool. It:

- **Exchanges UDP heartbeats** with its peers to track who's alive.
- **Runs health checks** so an unhealthy owner steps aside.
- **Elects an owner** by priority and runs a continuous decision loop.
- **Moves the VIP** with guarded `iproute2` / `arping` operations and gratuitous ARP so the
  network learns the new location fast.
- **Exposes an optional read-only local API** for status, backing an optional web dashboard.

**Safety first.** Real VIP changes are gated behind two independent controls — runtime dry-run
mode and `mutation_safety.allow_network_mutation` — both off by default. You can run the full
decision loop in dry-run and watch what it *would* do before letting it touch the network.

**Highlights**

- 🪶 Single C++23 binary, one TOML config, zero runtime dependencies beyond `iproute2`/`arping`.
- 🔌 Ships a systemd unit and also runs under **OpenRC, runit, dinit, and s6**.
- 🔒 Dry-run by default; real mutation is explicit, opt-in, and auditable.
- 📊 Optional read-only Next.js dashboard (separate from the daemon, never privileged).
- 🐧 Targets Arch, Ubuntu, Debian, Fedora, RHEL, and Rocky Linux.

> **Project status:** pre-1.0 and under active development. The daemon builds, tests, and runs
> the full failover loop with opt-in quorum and self-fencing; remote/STONITH fencing is out of
> scope. Validate real VIP movement in a lab before trusting it in production — see the
> [Disclaimer](#disclaimer).

**Documentation** lives in [`docs/`](docs/). Start with the
[configuration reference](docs/config-reference.md), the
[Linux capability requirements](docs/linux-capabilities.md) for real VIP movement, and the
[failover safety notes](docs/failover-safety.md).

## Installation

### Quick try (no changes to your host)

```sh
./scripts/build.sh
./build/easy-failover --config configs/easy-failover.toml --validate-config
./build/easy-failover --config configs/easy-failover.toml --dry-run   # never moves a VIP
```

### Install as a package (recommended)

Prebuilt `.deb` and `.rpm` packages are attached to each [GitHub release](https://github.com/EEkebin/easy-failover/releases) —
download one and install it with your package manager. Or build a package yourself; either way the
package manager pulls in the runtime dependencies (`iproute2`, `arping`):

```sh
./scripts/package.sh        # produces build-pkg/easy-failover_<ver>_amd64.deb (+ .rpm where rpmbuild exists)

# Debian / Ubuntu
sudo apt install ./build-pkg/easy-failover_*.deb

# Fedora / RHEL / Rocky
sudo dnf install ./build-pkg/easy-failover-*.rpm
```

Installing seeds `/etc/easy-failover/config.toml` from the example but does **not** auto-start the
service. Edit the node config, validate it, then enable:

```sh
sudoedit /etc/easy-failover/config.toml                      # set VIP, interface, peers
easy-failover --config /etc/easy-failover/config.toml --validate-config
sudo systemctl enable --now easy-failover.service            # only after the config is valid
```

### Build from source without packaging

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_SYSCONFDIR=/etc
cmake --build build
sudo cmake --install build
```

**Build dependencies:** a C++23 compiler (GCC or Clang), CMake, `make` or Ninja, Git. **Runtime
dependencies:** `iproute2` and `arping` (declared by the package; install manually for source builds).

**Not using systemd?** The packages target systemd, but the repo also ships OpenRC, runit, dinit, and
s6 service files for source installs — see the per-init guides for
[OpenRC](docs/service-openrc.md), [runit](docs/service-runit.md),
[dinit](docs/service-dinit.md), and [s6](docs/service-s6.md).

**Want the dashboard?** It's an optional read-only Next.js app shipped as a **separate**
`easy-failover-dashboard` package (its own `.deb`/`.rpm`, attached to releases or built with
`scripts/package-dashboard.sh`), running as an unprivileged service independent of the daemon —
see [running the dashboard](docs/dashboard-service.md).

## Uninstallation

Use your package manager. On removal, the service is stopped and — if this host currently owns the
VIP — the VIP is released from its interface.

```sh
# Debian / Ubuntu
sudo apt remove easy-failover         # keeps /etc/easy-failover (Debian convention)
sudo apt purge  easy-failover         # also removes /etc/easy-failover

# Fedora / RHEL / Rocky
sudo dnf remove easy-failover         # removes the package and /etc/easy-failover
```

## Contributing

Contributions are welcome. The `main` branch is protected; all changes land through a pull request
that passes the `Linux smoke build` CI check, and merges are squashed to keep history linear.

```sh
git checkout -b feature/my-change
# make your change, then:
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
git commit -am "Describe your change"
git push -u origin feature/my-change
```

Open a PR against `main` and reference any related issue. CI builds the project on Linux, runs the
test suite, and smoke-tests `--version`, config validation, and dry-run mode.

## Credits

Created and maintained by [**EEkebin**](https://github.com/EEkebin).

Built with these excellent open-source libraries:

- [spdlog](https://github.com/gabime/spdlog) — fast logging
- [CLI11](https://github.com/CLIUtils/CLI11) — command-line parsing
- [toml++](https://github.com/marzer/tomlplusplus) — TOML configuration parsing

The optional dashboard is built with [Next.js](https://nextjs.org/) and
[React](https://react.dev/).

## License

Licensed under the [Apache License 2.0](LICENSE). You may use, modify, and distribute this
software under its terms.

## Disclaimer

easy-failover performs privileged network operations and is **pre-1.0 software**. Moving a virtual
IP incorrectly can cause split-brain (two nodes claiming the same address) and service outages.
Opt-in **quorum** and **self-fencing** are implemented (a node releases the VIP when it loses quorum
or the election); **remote/STONITH fencing is not**. For a 2-node cluster, strict-majority quorum
means a lone survivor will not take over unless you add a witness node or set `election.quorum_size`
— see [`docs/failover-safety.md`](docs/failover-safety.md).

Real VIP mutation is disabled by default and must be explicitly enabled. **Do not enable it until
you have validated the node configuration, Linux capabilities, and failover behavior in your own
target environment.** The software is provided "as is", without warranty of any kind, to the extent
permitted by the Apache 2.0 license.
