# Distro Packaging Notes

easy-failover does not ship distribution-specific packages yet. These notes define the expected
package-maintainer contract for future Debian, RPM, and distro repository work.

## Scope

Future distro packages should package the existing CMake install output. They should not introduce a
separate file layout that differs from release tarballs or staged installs.

This document is guidance only. It does not add Debian packaging, RPM specs, package signing,
repository publishing, or distribution-specific CI.

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

The CMake install rules stage `config.example.toml`, not an active `config.toml`. A distro package
should install the example config as documentation or as a non-clobbering package-managed
configuration template, then create `/etc/easy-failover/config.toml` only through the distro's
normal conffile or admin-prompt mechanism.

Packages must not overwrite an operator's existing `/etc/easy-failover/config.toml` during install
or upgrade.

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

Do not add hard runtime dependencies for real VIP movement until packaged mutation support is enabled
and tested. While easy-failover remains non-mutating by default, package dependencies should stay
aligned with the actual packaged behavior.

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
