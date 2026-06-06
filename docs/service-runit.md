# Running easy-failover under runit

easy-failover ships a systemd unit by default, but it can also run under
[runit](http://smarden.org/runit/) on distributions such as Void Linux and Artix, or anywhere
runit supervises system services. This document covers installing, supervising, and operating the
agent with runit.

The runit service directory is generated from `packaging/runit/easy-failover/run.in` (and a
companion `log/run.in` logger) and installed by CMake alongside the systemd unit. runit supervises
the process directly, so the agent runs in the foreground with `--run-forever`.

## Install

The CMake install step writes a complete runit service directory. By default it is placed under
`${CMAKE_INSTALL_SYSCONFDIR}/sv`, which resolves to `/etc/sv` on a standard system install:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_SYSCONFDIR=/etc
cmake --build build
sudo cmake --install build --prefix /usr
```

That installs the executable service scripts at:

```text
/etc/sv/easy-failover/run
/etc/sv/easy-failover/log/run
```

Override the destination with `-DEASY_FAILOVER_RUNIT_DIR=<dir>` if your distribution keeps service
definitions elsewhere. Relative values are resolved against the install prefix; absolute values are
used as-is.

The binary and example config follow the same layout as every other install method:

```text
/usr/bin/easy-failover
/etc/easy-failover/config.example.toml
```

## Configuration

The `run` script starts the agent with:

```text
easy-failover --config /etc/easy-failover/config.toml --run-forever
```

Create the active config from the installed example before activating the service:

```sh
sudo install -d -m 0755 /etc/easy-failover
sudo cp /etc/easy-failover/config.example.toml /etc/easy-failover/config.toml
sudo $EDITOR /etc/easy-failover/config.toml
easy-failover --config /etc/easy-failover/config.toml --validate-config
```

See [`config-reference.md`](config-reference.md) for the full TOML schema. Keep
`mutation_safety.allow_network_mutation = false` until real VIP movement has been validated in your
environment, as described in [`failover-safety.md`](failover-safety.md).

The logger writes to `/var/log/easy-failover`, so create that directory first:

```sh
sudo install -d -m 0755 /var/log/easy-failover
```

## Start, Stop, Restart

Activate the service by symlinking it into the supervised service directory (commonly
`/var/service` or `/etc/service`, depending on the distribution). runit then starts it
automatically and restarts it if it exits:

```sh
sudo ln -s /etc/sv/easy-failover /var/service/
```

Control it with `sv`:

```sh
sudo sv status easy-failover
sudo sv restart easy-failover
sudo sv stop easy-failover
sudo sv start easy-failover
```

Deactivate the service by removing the symlink (this does not delete `/etc/sv/easy-failover`):

```sh
sudo sv stop easy-failover
sudo rm /var/service/easy-failover
```

## Logs

The companion `log/run` service pipes the agent's output through `svlogd`, which writes timestamped,
auto-rotated logs to `/var/log/easy-failover`. easy-failover emits stable logfmt-style runtime
events (`daemon_lifecycle_result` and `vip_operation`):

```sh
sudo tail -f /var/log/easy-failover/current
```

The `run` script redirects stderr to stdout (`exec 2>&1`) so both streams are captured by the
logger.

## Privileges and Capabilities

The `run` script runs the agent as `root` (the user runit itself runs as), matching the packaged
systemd unit. Real VIP movement requires the same privileges on any init system:

- `CAP_NET_ADMIN` for adding and removing the VIP with `ip addr`;
- `CAP_NET_RAW` for `arping` gratuitous ARP announcements;
- the `ip` (`iproute2`) and `arping` binaries available on `PATH`.

runit does not apply the systemd unit's sandboxing directives. To drop privileges to a dedicated
service account instead of running as `root`, wrap the agent invocation in `chpst -u <user>` inside
the `run` script and grant the binaries the required file capabilities. See
[`linux-capabilities.md`](linux-capabilities.md) for the full privilege and tooling notes, and
[`failover-safety.md`](failover-safety.md) for the safety controls that must remain in place before
enabling real network mutation.
