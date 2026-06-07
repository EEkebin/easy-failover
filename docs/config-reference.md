# Config Reference

easy-failover uses TOML configuration. The sample config lives at
`configs/easy-failover.toml`.

The minimal required config is the VIP plus the peer/server pool. Other fields have defaults and
can be overridden when needed.

## Minimal Config

```toml
[vip]
address = "10.0.0.50/24"
interface = "eth0"

[[peers]]
id = "node-b"
address = "10.0.0.12:7432"

[[peers]]
id = "node-c"
address = "10.0.0.13:7432"
```

## Top-Level Keys

```toml
node_id = "node-a"
priority = 100
```

- `node_id`: optional non-empty string that identifies this node. Defaults to the system hostname.
- `priority`: optional positive integer used by the simple election helper. Defaults to `100`.

## VIP

```toml
[vip]
address = "10.0.0.50/24"
interface = "eth0"
```

- `address`: required non-empty virtual IP address string; CIDR/prefix-length format (for example `10.0.0.50/24`) is recommended but not currently validated.
- `interface`: required non-empty Linux network interface name.

VIP movement is guarded by dry-run mode and the mutation safety config. By default, VIP manager
methods build dry-run `iproute2` and `arping` command requests. Real command requests require both a
non-dry-run runtime and explicit mutation safety opt-in. See
[`linux-capabilities.md`](linux-capabilities.md) for the Linux privileges required before enabling
real mutation on a host.

## Heartbeat

```toml
[heartbeat]
bind = "0.0.0.0:7432"
interval_ms = 1000
timeout_ms = 3000
```

- `bind`: optional non-empty bind address. Defaults to `0.0.0.0:7432`.
- `interval_ms`: optional positive heartbeat interval in milliseconds. Defaults to `1000`.
- `timeout_ms`: optional positive peer timeout in milliseconds. Defaults to `3000`.

The daemon uses UDP heartbeat datagrams on the configured bind address. Current production
transport support is IPv4 `host:port` only, such as `0.0.0.0:7432`, `127.0.0.1:7432`, or
`10.0.0.13:7432`. Peer addresses use the same IPv4 `host:port` format.

## Health

```toml
[health]
command = "curl -fsS http://127.0.0.1:8080/health"
interval_ms = 1000
timeout_ms = 2000
```

- `command`: optional health-check command. Defaults to empty, which means the node is treated as
  healthy until a health check is configured.
- `interval_ms`: optional positive health-check interval in milliseconds. Defaults to `1000`.
- `timeout_ms`: optional positive health-check timeout in milliseconds. Defaults to `2000`.

Linux health-check command execution exists behind an internal runner. It is not wired into a
daemon loop or failover decisions yet.

## Election

```toml
[election]
require_quorum = false
preempt = true
quorum_size = 0
```

- `require_quorum`: optional boolean. When `true`, a node may own the VIP only if it observes a
  quorum of the cluster (this node + peers with a fresh heartbeat). A node without quorum refuses to
  claim, and a master that loses quorum releases the VIP (self-fencing). Defaults to `false`.
- `preempt`: optional boolean. When `true` (default), a higher-priority node takes over from a
  lower-priority master. When `false`, it defers to an existing healthy master and only takes over
  once that master stops sending heartbeats (avoids failover flaps).
- `quorum_size`: optional integer overriding the quorum threshold. `0` (default) means automatic
  strict majority, `floor(N/2)+1`, where `N` is the configured cluster size (peers + 1). If set, it
  must be between `1` and `N`. Only used when `require_quorum` is `true`. Setting it below the
  majority weakens split-brain protection.

The election chooses the highest-priority healthy candidate and breaks ties by the lexicographically
lowest `node_id`. Quorum, preemption, and self-fencing layer on top of that result — see
[`failover-safety.md`](failover-safety.md), including the important note on **two-node clusters**
(strict majority means a lone survivor cannot own the VIP; add a witness node or set `quorum_size`).

## API

```toml
[api]
enabled = false
bind = "127.0.0.1:8743"
read_only = true
auth_token_file = ""
```

- `enabled`: optional boolean. Defaults to `false`.
- `bind`: optional non-empty string. Defaults to `127.0.0.1:8743` and is required when
  `enabled = true`.
- `read_only`: optional boolean. Defaults to `true`. Write mode (`read_only = false`) is opt-in and
  fails closed: API startup is rejected unless `auth_token_file` points at a readable, non-empty
  token file.
- `auth_token_file`: optional path to a file holding the bearer token that authenticates write
  requests. Only required when `read_only = false`. The file should be readable only by the daemon
  user (for example mode `0600`). The token is loaded into memory at startup and is never returned
  by the API or written to logs; `GET /api/v1/config` exposes only whether a token file is
  configured, never its contents.

The local API opens a loopback HTTP listener only when `enabled = true`. In read-only mode it serves
the read endpoints unauthenticated. In write mode it additionally serves the authenticated
`POST /api/v1/config/apply` endpoint, which validates a submitted config and atomically replaces the
active config (keeping a `config.toml.bak` backup); the change takes effect on the next daemon
restart. The full endpoint shape is documented in [`local-api-design.md`](local-api-design.md) and
the auth model in [`write-api-design.md`](write-api-design.md).

## Mutation Safety

```toml
[mutation_safety]
allow_network_mutation = false
```

- `allow_network_mutation`: optional boolean that permits non-dry-run VIP manager command requests
  only when the runtime is also not in dry-run mode. Defaults to `false`.

The default is intentionally safe: real network mutation is not permitted unless an operator
explicitly opts in and starts the daemon without `--dry-run`. CLI/runtime dry-run remains an
overriding safety control even when this setting is `true`. The installed systemd unit runs without
`--dry-run`, so keep this setting `false` until real VIP movement has been tested in the target
environment.

When real mutation is enabled, the runtime still waits through one successful heartbeat cycle before
allowing real VIP add/remove/announce operations. Heartbeat send or receive errors fail closed
before real VIP mutation.

## Peers

```toml
[[peers]]
id = "node-b"
address = "10.0.0.12:7432"
```

- `id`: required non-empty peer node identifier.
- `address`: required non-empty peer heartbeat IPv4 `host:port` address.

Each peer entry must be a TOML table.
At least one peer is required.

## Validation Summary

Config validation currently checks:

- `node_id` is not empty after applying the hostname default.
- `priority` is positive.
- `vip.address` and `vip.interface` are not empty.
- `heartbeat.bind` is not empty.
- `heartbeat.interval_ms` and `heartbeat.timeout_ms` are positive.
- `health.interval_ms` and `health.timeout_ms` are positive.
- at least one peer is configured.
- each peer has a non-empty `id` and `address`.
- `api.bind` is not empty when `api.enabled = true`.

Missing `[vip]` fails during config loading. `[heartbeat]`, `[health]`, `[election]`, `[api]`, and
`[mutation_safety]` may be omitted and will use defaults.
