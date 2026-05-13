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

No real VIP movement is implemented yet. Current VIP manager methods only log intended actions.

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

Real heartbeat networking is not implemented yet.

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
```

- `require_quorum`: optional boolean reserved for future quorum behavior. Defaults to `false`.
- `preempt`: optional boolean reserved for future priority preemption behavior. Defaults to `true`.

Current election logic only chooses the highest-priority healthy candidate and breaks ties by the
lexicographically lowest `node_id`.

Quorum and split-brain protections are design requirements before real VIP movement is implemented.
See [`failover-safety.md`](failover-safety.md) for the current safety notes.

## API

```toml
[api]
enabled = false
bind = "127.0.0.1:8743"
read_only = true
```

- `enabled`: optional boolean. Defaults to `false`.
- `bind`: optional non-empty string. Defaults to `127.0.0.1:8743` and is required when
  `enabled = true`.
- `read_only`: optional boolean reserved for the future API. Defaults to `true`.

The local API is not implemented yet.

## Peers

```toml
[[peers]]
id = "node-b"
address = "10.0.0.12:7432"
```

- `id`: required non-empty peer node identifier.
- `address`: required non-empty peer heartbeat address.

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

Missing `[vip]` fails during config loading. `[heartbeat]`, `[health]`, `[election]`, and `[api]`
may be omitted and will use defaults.
