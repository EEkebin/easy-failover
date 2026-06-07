# Failover Safety Notes

easy-failover must avoid moving a virtual IP until the agent has enough safety controls to reduce
split-brain risk. The codebase sends UDP heartbeats, models local decisions, keeps real VIP command
execution behind explicit opt-in, and supports opt-in **quorum** with **self-fencing** (see below).
It does not perform remote/STONITH fencing.

## Split-Brain Risk

Split-brain happens when two or more nodes believe they should own the same VIP at the same time.
For a VIP failover agent, that can route traffic to the wrong host, create ARP or neighbor cache
conflicts, and hide service failures behind inconsistent ownership.

The election helper chooses the highest-priority healthy candidate known to the local process.
Quorum and self-fencing (below) layer on top of that result so an isolated node does not serve from
a minority partition.

## Required Guardrails Before VIP Movement

Before easy-failover performs real network changes, the runtime should require:

- fresh heartbeat state for peers, including clear timeout handling;
- local health-check state, with failed or timed-out health checks preventing ownership;
- explicit handling for unknown peer state instead of assuming absent peers are safe;
- dry-run support for every privileged VIP action;
- operator opt-in before enabling real VIP mutation;
- structured logging for every decision that could change ownership.

The first real VIP backend should still be conservative. It should shell out to `iproute2` and
`arping` only behind explicit safety controls. A later native netlink backend can replace shelling
out after the behavior is well covered. The expected Linux privileges are documented in
[`linux-capabilities.md`](linux-capabilities.md).

After adding a VIP, the Linux backend sends both ARP announcement and unsolicited ARP update packets
with `arping` to reduce client neighbor-cache delay. This improves convergence, but real deployments
should still validate client, switch, and hypervisor behavior in the target network.

The current daemon uses a startup warmup before real mutation: when
`mutation_safety.allow_network_mutation = true` and the runtime is not in `--dry-run`, the first
successful heartbeat cycle warms the daemon up, and real VIP operations can only happen on a later
iteration. Heartbeat send or receive errors fail closed before real VIP mutation.

## Quorum

`election.require_quorum` enables quorum enforcement. It is **off by default**; enabling it changes
ownership behavior, so opt in deliberately.

When enabled, a node may own the VIP only if it can observe a **quorum** of the cluster. Observed
membership is counted as **this node plus every peer with a fresh heartbeat** (within
`heartbeat.timeout_ms`). Reachability is what matters here, not health: a reachable-but-unhealthy
peer still proves this node is not partitioned.

The required count is a **strict majority** of the configured cluster size `N` (peers + 1):

```
threshold = floor(N / 2) + 1
```

`election.quorum_size` overrides this with an explicit value (`0` = automatic majority). It must be
between `1` and `N`; setting it below the majority weakens split-brain protection and is the
operator's responsibility.

Behavior when quorum is enabled:

- A node **without quorum** refuses to claim the VIP (`stay_backup`, reason `no quorum ...`).
- A node that **holds the VIP and loses quorum** demotes and releases it (`become_backup`, reason
  `quorum lost ...`) — see Fencing below.

**Two-node clusters.** With strict majority, `N = 2` requires `threshold = 2`, so a lone survivor
cannot own the VIP: if the peer fails (or the link breaks) the survivor will **not** take over. This
is intentional — it prevents split-brain — but it means a plain 2-node cluster gives up automatic
failover. To get both failover and split-brain safety, add a **third (witness) node** so a single
failure still leaves a majority. As an explicit, riskier escape hatch, `election.quorum_size = 1`
lets a lone node own the VIP (re-accepting split-brain risk on a partition). Leaving
`require_quorum = false` keeps the original non-quorum behavior.

## Fencing and Operator Safeguards

easy-failover performs **self-fencing**: a node actively gives up the VIP when it must not own it.
This composes the ownership probe (which detects whether this host actually holds the VIP) with the
decision actions:

- **On demotion** — when a higher-priority healthy peer wins the election, the local master releases
  the VIP (`become_backup` → `ip addr del`).
- **On quorum loss** — when `require_quorum` is on and the node can no longer see a majority, it
  releases the VIP rather than serve from a possible minority partition.

Releases only run when the ownership probe confirms this host currently holds the VIP, and remain
behind the dry-run / `allow_network_mutation` / heartbeat-warmup gates. Every ownership-affecting
decision is logged with a `failover_reason` field for auditability.

**Preemption.** `election.preempt` (default `true`) controls whether a higher-priority node displaces
a working lower-priority master. With `preempt = false`, a node that comes up outranking the current
master does **not** take over while that master is healthy and sending heartbeats; it only takes over
once the master stops (its heartbeat goes stale). This avoids unnecessary failover flaps.

**Remote fencing (STONITH)** — actively powering off or isolating a peer via external hardware/cloud
APIs — is **out of scope**. Self-fencing plus quorum covers the isolated-node case; environments that
need hard guarantees against a misbehaving node that ignores its own decisions should add external
fencing at the infrastructure layer.

Any remote API that can affect ownership must require authentication, be explicitly enabled, and
avoid unauthenticated remote write access. Privileged actions are not exposed on a network listener
by default.

The first local API shape is read-only and disabled by default. Its endpoint design is documented
in [`local-api-design.md`](local-api-design.md).

## Current Status

Default behavior is intentionally non-mutating:

- election and failover decision helpers are pure local logic;
- quorum (`election.require_quorum`, off by default) and non-preemptive election
  (`election.preempt`) are enforced in the decision layer; an isolated node refuses to claim and a
  master that loses quorum self-fences (releases the VIP);
- the daemon sends and receives IPv4 UDP heartbeat datagrams for configured peers;
- Linux VIP manager methods build `iproute2` and multi-form `arping` command requests through a
  dry-run command runner by default;
- the daemon lifecycle runs VIP operations in dry-run mode unless both runtime dry-run is disabled
  and `mutation_safety.allow_network_mutation` is `true`;
- runtime lifecycle and VIP operation observations are logged as stable `key=value` events for
  auditability;
- SIGINT/SIGTERM handling records a shutdown request for the long-running runtime loop to observe;
- `--dry-run` does not change system state;
- real mutation waits for a successful heartbeat warmup cycle and fails closed on heartbeat
  transport errors;
- real VIP mutation still needs careful lab validation before production use.
