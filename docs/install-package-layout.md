# Install and Package Layout

easy-failover is still early WIP. Packaging should stage files under an explicit prefix or package
root first, then let the operator or distro package manager install them onto the host.

## CMake Install Layout

The CMake install rules stage these paths by default:

```text
bin/easy-failover
etc/easy-failover/config.example.toml
lib/systemd/system/easy-failover.service
share/doc/easy-failover/README.md
share/doc/easy-failover/LICENSE
share/doc/easy-failover/docs/*.md
```

For a non-root local staging check:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix "$PWD/stage"
```

The staged config is named `config.example.toml` on purpose. Install rules must not overwrite an
operator's active `/etc/easy-failover/config.toml`.

The systemd unit directory is controlled by `EASY_FAILOVER_SYSTEMD_UNIT_DIR`, defaulting to
`lib/systemd/system`. This is intentionally separate from `CMAKE_INSTALL_LIBDIR` because systemd
unit paths are not library-architecture paths.

Release tarballs are built from this same staged CMake install tree so their internal layout matches
the install rules.

## Production Paths

The intended Linux package layout is:

```text
/usr/bin/easy-failover
/etc/easy-failover/config.toml
/usr/lib/systemd/system/easy-failover.service
/usr/share/doc/easy-failover/
```

Package scripts should copy or prompt from the example config into
`/etc/easy-failover/config.toml` instead of replacing an existing active config.

## Safety Notes

The packaged systemd unit currently runs as `root` and keeps hardening as TODOs. Do not tighten or
grant capabilities in packaging until real VIP movement is implemented and validated on target
distributions.

The default sample config keeps the local API disabled and does not enable real network mutation.
Packaging should not change those safety defaults.
