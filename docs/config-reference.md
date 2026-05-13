# Config Reference

easy-failover uses TOML configuration. The sample config lives at
`configs/easy-failover.toml`.

The current implementation intentionally keeps production configs explicit. Do not rely on broad
defaults yet; required operational fields should be present in the file.

## Top-Level Keys

```toml
node_id = "node-a"
priority = 100
```

- `node_id`: required non-empty string that identifies this node.
- `priority`: required positive integer used by the simple election helper.

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

- `bind`: required non-empty bind address.
- `interval_ms`: required positive heartbeat interval in milliseconds.
- `timeout_ms`: required positive peer timeout in milliseconds.

Real heartbeat networking is not implemented yet.

## Health

```toml
[health]
command = "curl -fsS http://127.0.0.1:8080/health"
interval_ms = 1000
timeout_ms = 2000
```

- `command`: required non-empty health-check command.
- `interval_ms`: required positive health-check interval in milliseconds.
- `timeout_ms`: required positive health-check timeout in milliseconds.

Health-check command execution is not implemented yet.

## Election

```toml
[election]
require_quorum = false
preempt = true
```

- `require_quorum`: boolean reserved for future quorum behavior.
- `preempt`: boolean reserved for future priority preemption behavior.

Current election logic only chooses the highest-priority healthy candidate and breaks ties by the
lexicographically lowest `node_id`.

## API

```toml
[api]
enabled = false
bind = "127.0.0.1:8743"
read_only = true
```

- `enabled`: boolean; the future local API should remain disabled by default.
- `bind`: required non-empty string only when `enabled = true`.
- `read_only`: boolean reserved for the future API.

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

## Validation Summary

Config validation currently checks:

- `node_id` is not empty.
- `priority` is positive.
- `vip.address` and `vip.interface` are not empty.
- `heartbeat.bind` is not empty.
- `heartbeat.interval_ms` and `heartbeat.timeout_ms` are positive.
- `health.command` is not empty.
- `health.interval_ms` and `health.timeout_ms` are positive.
- each peer has a non-empty `id` and `address`.
- `api.bind` is not empty when `api.enabled = true`.

Missing required TOML tables fail during config loading.
