# LAN Auto-Discovery (self-forming failover pool)

Goal: install easy-failover on a set of boxes on the same LAN and have them **find each other and
form a failover pool automatically** — no static `[[peers]]`, no mandatory SSH onboarding. One node
becomes master by priority; identity is stable (MAC-based); membership is authenticated so a rogue
host can't join or steal the VIP.

This replaces "configure every peer by hand" with a zero-config beacon protocol, opt-in via
`[discovery]` (see [`config-reference.md`](config-reference.md)).

## Model

- Each node periodically **broadcasts a beacon** (UDP, ~1/sec) carrying: cluster name, its **MAC**
  (identity), `node_id`, **priority**, advertised contact address, health, current state, and a
  sequence number.
- Every node listens and builds a **live peer table** keyed by MAC; entries expire when no fresh
  beacon arrives within `timeout_ms`. So the beacon doubles as discovery *and* liveness — a node
  that goes silent simply drops out of the election.
- The existing election runs over that live set: **master = highest-priority healthy node**, with
  the current preempt/quorum rules unchanged. "Make this node master" = give it the top priority.
- Priority is per-node (set at install or from the dashboard/cockpit) and travels in the beacon,
  associated with the node's MAC.

## Security

Beacons are authenticated with **HMAC-SHA256** over a **shared cluster secret** (`secret_file`). A
receiver drops any beacon whose HMAC doesn't verify or whose cluster name doesn't match, so only
nodes holding the secret can join the pool or contend for the VIP. The secret is mandatory when
discovery is enabled (fail closed). Distribute it by copying the key file to each node (the SSH
onboarding flow can seed it automatically). The HMAC compare is constant-time.

Known limitation (future hardening): a captured beacon could be replayed within the timeout window
to briefly keep a dead node "alive". A monotonic timestamp + replay window will close this; the
sequence number is already carried for it.

## Implementation status

Built and unit-tested (pure, no sockets):

- `src/discovery/Hmac.{hpp,cpp}` — vendored SHA-256 + HMAC-SHA256 (no crypto-lib dependency),
  verified against the RFC 4231 known-answer vector; constant-time compare.
- `src/discovery/Beacon.{hpp,cpp}` — the beacon struct + canonical signed wire format
  (`signBeacon` / `verifySignedBeacon`), failing closed on bad signature, cluster mismatch, version
  skew, or malformed input.
- `src/discovery/PeerTable.{hpp,cpp}` — the live roster: observe verified beacons, exclude self,
  expire stale entries, project to the decision layer's `PeerStatus`.
- `[discovery]` config + validation.

Also built and wired (phase 2):

- `src/discovery/LinuxUdpDiscoveryTransport.{hpp,cpp}` — a UDP broadcast socket
  (SO_REUSEADDR/REUSEPORT/BROADCAST, non-blocking drain, broadcast to 255.255.255.255); degrades to
  a no-op if the socket can't bind, so it never crashes the daemon.
- `src/platform/MacAddress.hpp` + `LinuxMacAddress.cpp` — reads the VIP interface's MAC for identity.
- Runtime loop wiring (`DiscoveryContext`): each tick the daemon drains + verifies beacons into the
  peer table, broadcasts its own signed beacon on the interval, and folds the discovered peers into
  the election. Opt-in (`[discovery].enabled` + a transport), so existing behavior is unchanged.
- `main.cpp` constructs the transport, loads the secret (fail-closed if empty), reads the MAC, and
  passes the context to the loop.

Also built (phase 3, pool view):

- The daemon exposes the observed pool at `GET /api/v1/status` as a `pool` array
  (`node_id`/`priority`/`healthy`/`state`), sourced from the merged peer set used by the election.
- The dashboard and the Cockpit plugin render it as the **failover pool** (this node + the observed
  members, each with role, priority, health).

Remaining (follow-up PRs):

1. **Set priority / designate master from the UI** — editing a *remote* node's priority needs that
   node's write token in the dashboard roster (a per-node token store); the local node's priority is
   already editable via the config editor.
2. **Cockpit "add node"** — SSH-install on a new host (it then auto-joins); seed the cluster secret.
