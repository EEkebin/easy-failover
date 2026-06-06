# Running easy-failover under dinit

easy-failover ships a systemd unit by default, but it can also run under
[dinit](https://davmac.org/projects/dinit/) on distributions such as Chimera Linux and Artix, or
anywhere dinit supervises system services. This document covers installing, supervising, and
operating the agent with dinit.

The dinit service description is generated from `packaging/dinit/easy-failover.in` and installed by
CMake alongside the systemd unit. dinit supervises the process directly, so the agent runs in the
foreground with `--run-forever`.

## Install

The CMake install step writes the dinit service description. By default it is placed under
`${CMAKE_INSTALL_SYSCONFDIR}/dinit.d`, which resolves to `/etc/dinit.d` on a standard system
install:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_SYSCONFDIR=/etc
cmake --build build
sudo cmake --install build --prefix /usr
```

That installs the service description at:

```text
/etc/dinit.d/easy-failover
```

Override the destination with `-DEASY_FAILOVER_DINIT_DIR=<dir>` if your system keeps service
descriptions elsewhere. Relative values are resolved against the install prefix; absolute values are
used as-is.

The binary and example config follow the same layout as every other install method:

```text
/usr/bin/easy-failover
/etc/easy-failover/config.example.toml
```

## Configuration

The service description runs the agent with:

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

By default the service writes its output to `/var/log/easy-failover.log` (the `logfile` setting). The
installed description also includes a commented `depends-on = network` line; uncomment and set it to
your system's networking service name so the agent starts only after the network is up.

## Start, Stop, Restart

Control the service with `dinitctl`. When dinit is the system init, use the system socket; otherwise
target the relevant dinit instance:

```sh
sudo dinitctl start easy-failover
sudo dinitctl status easy-failover
sudo dinitctl restart easy-failover
sudo dinitctl stop easy-failover
```

To start the agent automatically at boot, add it to your system's boot service dependencies, for
example by enabling it:

```sh
sudo dinitctl enable easy-failover
```

`dinitctl enable` persists the service in the boot set; `dinitctl disable easy-failover` removes it.

## Logs

The service description sets `logfile = /var/log/easy-failover.log`, so dinit redirects the agent's
output there. easy-failover emits stable logfmt-style runtime events (`daemon_lifecycle_result` and
`vip_operation`):

```sh
sudo tail -f /var/log/easy-failover.log
```

dinit itself records service start/stop/restart activity through its own logging.

## Privileges and Capabilities

The system dinit instance runs services as `root`, matching the packaged systemd unit. Real VIP
movement requires the same privileges on any init system:

- `CAP_NET_ADMIN` for adding and removing the VIP with `ip addr`;
- `CAP_NET_RAW` for `arping` gratuitous ARP announcements;
- the `ip` (`iproute2`) and `arping` binaries available on `PATH`.

dinit does not apply the systemd unit's sandboxing directives. To drop privileges to a dedicated
service account instead of running as `root`, set `run-as = <user>` in the service description and
grant the binaries the required file capabilities. See
[`linux-capabilities.md`](linux-capabilities.md) for the full privilege and tooling notes, and
[`failover-safety.md`](failover-safety.md) for the safety controls that must remain in place before
enabling real network mutation.
