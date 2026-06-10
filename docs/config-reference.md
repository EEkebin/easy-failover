# Config Reference

easy-failover uses TOML configuration. The sample config lives at
`configs/easy-failover.toml`.

The shipped config is a **clean slate**: no VIP and no peers. The daemon starts and idles until you
configure it — set the VIP and add peers via the dashboard's onboarding, or edit the config directly
and restart. Every field has a default, so an empty config is valid (and idle).

## Clean-Slate Config (shipped default)

```toml
node_id = "node-a"
priority = 100

[vip]
address = ""    # leave empty to stay unconfigured / idle
interface = ""

# No [[peers]] — added later via the dashboard or by editing this file.
```

## Minimal Working Config

To actually own a VIP and fail over, set the VIP and (optionally) add peers:

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

- `address`: the virtual IP address string; CIDR/prefix-length format (for example `10.0.0.50/24`) is recommended but not currently validated. May be left empty to stay unconfigured.
- `interface`: the Linux network interface name. May be left empty to stay unconfigured.

`address` and `interface` must be set **together**: setting one but not the other is rejected.
Leaving **both** empty is valid and means "unconfigured" — the daemon runs but idles (no VIP
operations) until a VIP is configured. This is the shipped default; configure the VIP via the
dashboard onboarding or by editing this file, then restart the daemon.

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
enabled = true
bind = "0.0.0.0:8743"
read_only = true
auth_token_file = ""
```

- `enabled`: optional boolean. Defaults to `true` (the dashboard reads this API).
- `bind`: optional non-empty string. Defaults to `0.0.0.0:8743` (all interfaces) and is required when
  `enabled = true`.
- `read_only`: optional boolean. Defaults to `true`. Write mode (`read_only = false`) is opt-in and
  fails closed: API startup is rejected unless `auth_token_file` points at a readable, non-empty
  token file.
- `auth_token_file`: optional path to a file holding the bearer token that authenticates write
  requests. Only required when `read_only = false`. The file should be readable only by the daemon
  user (for example mode `0600`). The token is loaded into memory at startup and is never returned
  by the API or written to logs; `GET /api/v1/config` exposes only whether a token file is
  configured, never its contents.

> **Exposure note — `0.0.0.0`.** The default binds the API to **all** interfaces so a dashboard on
> another host can reach it. That also means anything that can route to this host can reach the API.
> In read-only mode it exposes only status/config-shape (no token contents, no privileged actions),
> but if you do not want it reachable off-box, set `bind = "127.0.0.1:8743"` or firewall the port.

The local API opens an HTTP listener when `enabled = true`. In read-only mode it serves the read
endpoints unauthenticated. In write mode it additionally serves the authenticated
`POST /api/v1/config/apply` endpoint, which validates a submitted config and atomically replaces the
active config (keeping a `config.toml.bak` backup); the change takes effect on the next daemon
restart. The full endpoint shape is documented in [`local-api-design.md`](local-api-design.md) and
the auth model in [`write-api-design.md`](write-api-design.md).

### Enabling write mode (the "write token")

A **write token** is a shared secret: requests that change config must send it in an
`Authorization: Bearer <token>` header. It exists because the API ships read-only — write mode lets
the dashboard (or `curl`) apply config changes, including the VIP.

> **On a packaged install (`.deb`/`.rpm`) this is already done for you.** The installer generates a
> random token in `/etc/easy-failover/api.token`, flips the daemon to `read_only = false`, writes
> the same token into the dashboard's env (`EASY_FAILOVER_TOKEN_LOCAL`), and creates a roster entry
> for the local node pointing at it — so the bundled dashboard's **Apply** button works out of the
> box. The dashboard binds **localhost** by default (it is unauthenticated and now write-capable);
> reach it remotely with an SSH tunnel. See [`dashboard-service.md`](dashboard-service.md). The
> steps below are for **building/running from source**, where the API stays read-only until you opt
> in, or for rotating/replacing the token.

1. Generate a token and store it where only the daemon can read it:

   ```sh
   sudo install -d -m 0700 /etc/easy-failover
   openssl rand -hex 32 | sudo tee /etc/easy-failover/api.token >/dev/null
   sudo chmod 0600 /etc/easy-failover/api.token
   ```

2. Point the daemon at it and turn off read-only in `/etc/easy-failover/config.toml`:

   ```toml
   [api]
   enabled = true
   bind = "0.0.0.0:8743"
   read_only = false
   auth_token_file = "/etc/easy-failover/api.token"
   ```

   (If `read_only = false` but `auth_token_file` is missing or empty, the API refuses to start —
   it fails closed.)

3. Restart the daemon: `sudo systemctl restart easy-failover`.

4. Give the dashboard the same token. The dashboard reads it from an **environment variable** (its
   roster entry references the variable *name* via `tokenEnv`, never the token itself); set that
   variable in `/etc/easy-failover-dashboard/dashboard.env` and restart
   `easy-failover-dashboard`. See [`dashboard-service.md`](dashboard-service.md).

`curl` example for a write request:

```sh
TOKEN="$(sudo cat /etc/easy-failover/api.token)"
curl -fsS -X POST http://127.0.0.1:8743/api/v1/config/apply \
  -H "Authorization: Bearer ${TOKEN}" \
  -H 'Content-Type: application/json' \
  -d '{"format":"toml","config":"..."}'
```

## Mutation Safety

```toml
[mutation_safety]
allow_network_mutation = true
```

- `allow_network_mutation`: optional boolean that permits non-dry-run VIP manager command requests
  only when the runtime is also not in dry-run mode. Defaults to `true`.

Real network mutation is **on by default** so a configured node actually moves the VIP. This is
safe on a fresh install because the shipped config is unconfigured (no VIP), so the daemon idles and
performs **no** network operations until you set a VIP. CLI/runtime `--dry-run` remains an overriding
safety control even when this setting is `true`: start the daemon with `--dry-run` to rehearse
without touching the network. Set `allow_network_mutation = false` if you want a node that runs and
participates but never performs real VIP add/remove/announce operations.

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

Each peer entry must be a TOML table. Peers are **optional**: zero peers is valid (a clean-slate or
single-node config — the node simply has no failover partner). Add peers via the dashboard onboarding
or by editing this file. With **discovery** enabled (below), peers are learned automatically and you
usually don't list any here.

## Discovery (LAN auto-clustering)

```toml
[discovery]
enabled = false
cluster = "default"
bind = "0.0.0.0:7433"
interval_ms = 1000
timeout_ms = 3000
secret_file = "/etc/easy-failover/cluster.key"
```

When enabled, the node periodically broadcasts a small **beacon** on the LAN and learns of other
nodes from theirs, so a failover pool forms with **no static `[[peers]]`** — install the service on
each box with the same `cluster` and shared secret and they find each other.

- `enabled`: optional boolean. Defaults to `false`.
- `cluster`: pool name; only nodes with the same name (and secret) join each other.
- `bind`: `host:port` the beacon socket uses. Defaults to `0.0.0.0:7433`.
- `interval_ms` / `timeout_ms`: beacon period and how long a silent node stays in the pool before
  it expires out of the election.
- `secret_file`: path to a file holding the **shared cluster secret**. Every beacon is HMAC-SHA256
  signed with it, and nodes reject beacons that don't verify — so a rogue host on the LAN can't join
  the pool or hijack the VIP. **Required** when `enabled = true` (the daemon fails closed otherwise).
  Generate one with `openssl rand -hex 32` and copy the same file to every node in the pool.

> Status: the config, the signed-beacon protocol, and the dynamic peer table are implemented and
> tested; the broadcast socket + runtime wiring (so it discovers live) land in a follow-up. See
> [`discovery-design.md`](discovery-design.md).

## Validation Summary

Config validation currently checks:

- `node_id` is not empty after applying the hostname default.
- `priority` is positive.
- `vip.address` and `vip.interface` are set together (both empty = unconfigured/idle is valid;
  setting only one is rejected).
- `heartbeat.bind` is not empty.
- `heartbeat.interval_ms` and `heartbeat.timeout_ms` are positive.
- `health.interval_ms` and `health.timeout_ms` are positive.
- each peer has a non-empty `id` and `address` (zero peers is allowed).
- `api.bind` is not empty when `api.enabled = true`.

Missing `[vip]` fails during config loading. `[heartbeat]`, `[health]`, `[election]`, `[api]`, and
`[mutation_safety]` may be omitted and will use defaults.
