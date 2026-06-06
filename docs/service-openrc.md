# Running easy-failover under OpenRC

easy-failover ships a systemd unit by default, but it can also run under
[OpenRC](https://github.com/OpenRC/openrc) on distributions such as Alpine Linux, Gentoo, and
Artix. This document covers installing, supervising, and operating the agent with OpenRC.

The OpenRC init script is generated from `packaging/openrc/easy-failover.initd.in` and installed by
CMake alongside the systemd unit. It uses OpenRC's built-in `supervise-daemon` supervisor, so the
agent is restarted automatically if it exits.

## Install

The CMake install step writes the init script to the OpenRC service directory. By default it is
placed under `${CMAKE_INSTALL_SYSCONFDIR}/init.d`, which resolves to `/etc/init.d` on a standard
system install:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_SYSCONFDIR=/etc
cmake --build build
sudo cmake --install build --prefix /usr
```

That installs the executable init script at:

```text
/etc/init.d/easy-failover
```

Override the destination with `-DEASY_FAILOVER_OPENRC_DIR=<dir>` if your distribution uses a
different path. Relative values are resolved against the install prefix; absolute values are used
as-is.

The binary and example config follow the same layout as every other install method:

```text
/usr/bin/easy-failover
/etc/easy-failover/config.example.toml
```

## Configuration

The init script runs the agent with:

```text
easy-failover --config /etc/easy-failover/config.toml --run-forever
```

Create the active config from the installed example before starting the service:

```sh
sudo install -d -m 0755 /etc/easy-failover
sudo cp /etc/easy-failover/config.example.toml /etc/easy-failover/config.toml
sudo $EDITOR /etc/easy-failover/config.toml
easy-failover --config /etc/easy-failover/config.toml --validate-config
```

See [`config-reference.md`](config-reference.md) for the full TOML schema. Keep
`mutation_safety.allow_network_mutation = false` until real VIP movement has been validated in your
environment, as described in [`failover-safety.md`](failover-safety.md).

## Start, Stop, Restart

Add the service to the default runlevel so it starts on boot, then control it with `rc-service`:

```sh
sudo rc-update add easy-failover default
sudo rc-service easy-failover start
sudo rc-service easy-failover status
sudo rc-service easy-failover restart
sudo rc-service easy-failover stop
```

Remove it from the runlevel with:

```sh
sudo rc-update del easy-failover default
```

## Logs

The init script routes the agent's stdout/stderr to syslog through `logger` under the `daemon`
facility with the tag `easy-failover`. easy-failover emits stable logfmt-style runtime events
(`daemon_lifecycle_result` and `vip_operation`), so they land in your syslog daemon's output:

```sh
# busybox syslog / syslog-ng / rsyslog destination varies by distribution
sudo tail -f /var/log/messages | grep easy-failover
```

The supervisor itself logs start/stop/respawn activity through OpenRC.

## Privileges and Capabilities

The init script runs the agent as `root`, matching the packaged systemd unit. Real VIP movement
requires the same privileges on any init system:

- `CAP_NET_ADMIN` for adding and removing the VIP with `ip addr`;
- `CAP_NET_RAW` for `arping` gratuitous ARP announcements;
- the `ip` (`iproute2`) and `arping` binaries available on `PATH`.

OpenRC does not apply the systemd unit's sandboxing directives. If you need to drop privileges to a
dedicated service account with file capabilities instead of running as `root`, set `user`/`group`
in the init script and grant the binaries the required capabilities. See
[`linux-capabilities.md`](linux-capabilities.md) for the full privilege and tooling notes, and
[`failover-safety.md`](failover-safety.md) for the safety controls that must remain in place before
enabling real network mutation.
