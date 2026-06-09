# Distro Packaging Notes

easy-failover builds a single `.deb`/`.rpm` (daemon **and** dashboard) via CPack, driven from the
CMake `install()` rules. Build it with `scripts/package.sh` (needs a C++ toolchain plus Node.js/npm
for the bundled dashboard; the `.deb` needs `dpkg-deb`, the `.rpm` needs `rpmbuild`). Set
`EASY_FAILOVER_NO_DASHBOARD=1` for a daemon-only package. These notes record the layout and the
package lifecycle.

## Versioning and releases

- **Rolling:** every push to `main` runs `.github/workflows/release.yml`, which builds packages for
  every architecture and updates a `latest` pre-release. The version is calendar-based:
  `YYYY.MM.DD.<build>` where `<build>` is `git rev-list --count HEAD` (`scripts/version.sh`). The
  count is strictly monotonic, so `apt`/`dnf` always upgrade to the newest rolling build; the short
  commit hash is in the release title/notes for traceability (it is *not* in the orderable version,
  because a hex hash does not sort monotonically).
- **Stable:** pushing a `vX.Y.Z` tag publishes a normal release with version `X.Y.Z`.
- Override the version on any build with `PKG_VERSION=<v> scripts/package.sh`.

## Architectures

- **amd64** and **arm64** — built natively (GitHub `ubuntu-24.04` / `ubuntu-24.04-arm` runners), the
  full combined package (`.deb` and `.rpm`).
- **riscv64** — a **daemon-only `.deb`** (`EASY_FAILOVER_NO_DASHBOARD=1`), built **emulated** (QEMU,
  Debian container; there are no native riscv64 GitHub runners) and non-blocking in CI. The dashboard
  is excluded on riscv64: Next.js 16 ships no riscv64 native binding for Turbopack or `next-swc`, and
  the WASM `next-swc` fallback crashes (`RuntimeError: unreachable`) under emulated riscv64. Revisit
  if upstream publishes a riscv64 `@next/swc`.

## Scope

The package reuses the CMake install output via CPack components, grouped into one package: the
`daemon` component (binary, example config, systemd unit, docs) and the `dashboard` component
(standalone Next.js server, its unit, env example). The OpenRC/runit/dinit/s6 service files are their
own components, kept out of the package but available for source installs via
`cmake --install --component <name>`. The source tarball ships the `daemon` component only.

Not yet covered here: official distro-repository submission (lintian/rpmlint-clean `debian/` and
`.spec` sources), package signing, and repository publishing.

## Lifecycle (maintainer scripts)

- **install** (`postinst` / `%post`): seed `/etc/easy-failover/config.toml` from the shipped example
  if absent; `systemctl daemon-reload`. The service is **not** auto-started — edit and validate the
  config first.
- **remove**: the service is stopped and, if this host owns the VIP, it is released
  (`easy-failover --release-vip`, best-effort).
- **config removal**: `apt purge` removes `/etc/easy-failover` on Debian/Ubuntu (plain `apt remove`
  keeps it, per Debian convention); on RPM, erase removes it.

## Installed Files

The intended packaged Linux layout is:

```text
/usr/bin/easy-failover
/etc/easy-failover/config.toml
/usr/lib/systemd/system/easy-failover.service
/usr/share/doc/easy-failover/README.md
/usr/share/doc/easy-failover/LICENSE
/usr/share/doc/easy-failover/docs/*.md
```

The CMake install rules stage `config.example.toml`, not an active `config.toml`. The shipped
maintainer scripts (`packaging/deb/postinst`, `packaging/rpm/postinst.sh`) seed
`/etc/easy-failover/config.toml` from the example on first install **only when it does not already
exist**, so an operator's edited `config.toml` is never overwritten on reinstall or upgrade. The
active `config.toml` is intentionally not tracked in the package file database; `apt purge` (Debian)
or erase (RPM) removes the whole `/etc/easy-failover` directory.

## Build and Staging

Package builds should use the CMake install rules as the source of truth:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_SYSCONFDIR=/etc
cmake --build build
DESTDIR="$pkgdir" cmake --install build --prefix /usr
```

`DESTDIR` must point at the distro package root. `--prefix /usr` keeps the binary and docs in normal
system package locations while `CMAKE_INSTALL_SYSCONFDIR=/etc` keeps configuration under `/etc`.

The systemd unit install directory is controlled by `EASY_FAILOVER_SYSTEMD_UNIT_DIR`, which defaults
to `lib/systemd/system` relative to the install prefix. Keep this value relative so package staging
and release tarball creation continue to work.

## Runtime Dependencies

The current runtime requires a modern Linux userspace and the C++ runtime needed by the compiler used
for the package build. Deployments that enable real VIP movement also require:

- `ip` from `iproute2`;
- `arping`, commonly from `iputils` or `iputils-arping` depending on the distribution, for both
  ARP announcement and unsolicited ARP update packets;
- systemd when installing and enabling the packaged service unit.

The packages declare these as hard runtime dependencies so the package manager installs them
alongside the daemon: Debian/Ubuntu `Depends: iproute2, iputils-arping`; RPM `Requires: iproute,
iputils`. The daemon's `arping -A/-U -I <iface>` invocation is the **iputils** arping CLI, so the
dependency is `iputils-arping` — not the conflicting Thomas-Habets `arping` package, which uses
different flags (`-i` for the interface).
They are required for any host that performs real VIP movement; declaring them up front avoids a
runtime failure the first time mutation is enabled.

## systemd Service Lifecycle

Packages may install `easy-failover.service` into the system unit directory, but post-install scripts
should not enable or start it automatically without an operator decision. The daemon needs a valid
node-specific config and deployment-specific failover safety review before it should run.

Recommended service lifecycle behavior:

- install the unit file;
- reload the systemd daemon after installing or upgrading the unit;
- leave service enablement to the operator or distro policy;
- avoid starting the service during package install unless the package manager has an explicit
  service-management policy for configured services.

The packaged unit currently runs as `root` but includes baseline hardening and a constrained
capability set. Distro packaging should preserve those defaults unless the target distribution has
validated a different service user, capability model, or command backend.

## Capabilities and Privilege

Real VIP movement is expected to need `CAP_NET_ADMIN` for interface address changes and
`CAP_NET_RAW` for ARP announcement tooling. The packaged unit grants only those capabilities while
still running as `root`; a future dedicated service user remains the target operating model once it
has been validated across distributions.

Before a package grants file capabilities, ambient capabilities, or a dedicated service user, test
the exact distribution builds of `iproute2`, `arping`, systemd, and easy-failover together. Some
`arping` variants rely on file capabilities or setuid behavior differently.

Capability and hardening guidance is tracked in
[`linux-capabilities.md`](linux-capabilities.md).

## Debian and RPM Considerations

Debian-family packages should treat `/etc/easy-failover/config.toml` as administrator-owned
configuration and avoid replacing it on upgrade. RPM-family packages should use equivalent config
file handling so local edits survive upgrades.

Both packaging families should:

- build from source with the project CMake rules;
- package documentation under the distro's normal documentation path for `easy-failover`;
- install the systemd unit through distro-supported unit macros or maintainer-script helpers;
- avoid enabling network mutation through package defaults;
- preserve the sample config's safe defaults, including the disabled local API.

Release tarballs remain a generic staged install artifact. Distro packages should build from source
or from a source archive, not repackage the binary release tarball as the primary packaging flow.
