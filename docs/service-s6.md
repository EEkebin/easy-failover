# Running easy-failover under s6

easy-failover ships a systemd unit by default, but it can also run under
[s6](https://skarnet.org/software/s6/) supervision, whether managed directly by `s6-svscan` or
declared as an `s6-rc` longrun. This document covers installing, supervising, and operating the
agent with s6.

The s6 service directory is generated from `packaging/s6/easy-failover/run.in` (and a companion
`log/run.in` logger) and installed by CMake alongside the systemd unit. s6-supervise runs the
process directly, so the agent runs in the foreground with `--run-forever`.

## Install

The CMake install step writes a complete s6 service directory. By default it is placed under
`${CMAKE_INSTALL_SYSCONFDIR}/s6`, which resolves to `/etc/s6` on a standard system install:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_SYSCONFDIR=/etc
cmake --build build
sudo cmake --install build --prefix /usr
```

That installs the executable service scripts at:

```text
/etc/s6/easy-failover/run
/etc/s6/easy-failover/log/run
```

Override the destination with `-DEASY_FAILOVER_S6_DIR=<dir>` to match your scan directory or
`s6-rc` source layout. Relative values are resolved against the install prefix; absolute values are
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

Create the active config from the installed example before supervising the service:

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

Place the service under an active scan directory so `s6-svscan`/`s6-supervise` supervises it
(commonly by symlinking it into your scan dir), then control it with `s6-svc`:

```sh
sudo ln -s /etc/s6/easy-failover /run/service/
sudo s6-svscanctl -a /run/service        # tell s6-svscan to pick up the new service

sudo s6-svstat /run/service/easy-failover
sudo s6-svc -r /run/service/easy-failover    # restart
sudo s6-svc -d /run/service/easy-failover    # down (stop)
sudo s6-svc -u /run/service/easy-failover    # up (start)
```

If you manage services with `s6-rc` instead, use the installed directory as a longrun source for
your compiled service database and start/stop it with `s6-rc -u change easy-failover` /
`s6-rc -d change easy-failover`. The exact scan-directory and `s6-rc` source paths vary by
distribution; adjust `EASY_FAILOVER_S6_DIR` and the commands above to match your setup.

## Logs

The companion `log/run` service runs `s6-log`. s6-supervise automatically connects the main
service's stdout to this logger, which writes rotated logs to `/var/log/easy-failover`.
easy-failover emits stable logfmt-style runtime events (`daemon_lifecycle_result` and
`vip_operation`):

```sh
sudo tail -f /var/log/easy-failover/current
```

The `run` script redirects stderr to stdout (`exec 2>&1`) so both streams reach the logger.

## Privileges and Capabilities

s6-supervise runs the service as the user the supervision tree runs as, normally `root`, matching
the packaged systemd unit. Real VIP movement requires the same privileges on any init system:

- `CAP_NET_ADMIN` for adding and removing the VIP with `ip addr`;
- `CAP_NET_RAW` for `arping` gratuitous ARP announcements;
- the `ip` (`iproute2`) and `arping` binaries available on `PATH`.

s6 does not apply the systemd unit's sandboxing directives. To drop privileges to a dedicated
service account instead of running as `root`, wrap the agent invocation with `s6-setuidgid <user>`
inside the `run` script and grant the binaries the required file capabilities. See
[`linux-capabilities.md`](linux-capabilities.md) for the full privilege and tooling notes, and
[`failover-safety.md`](failover-safety.md) for the safety controls that must remain in place before
enabling real network mutation.
