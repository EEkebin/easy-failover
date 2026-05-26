# Failover Safety Notes

easy-failover must avoid moving a virtual IP until the agent has enough safety controls to reduce
split-brain risk. The current codebase only models local decisions and builds non-mutating VIP
command requests. It does not send heartbeats, confirm quorum, fence peers, or change network
state.

## Split-Brain Risk

Split-brain happens when two or more nodes believe they should own the same VIP at the same time.
For a VIP failover agent, that can route traffic to the wrong host, create ARP or neighbor cache
conflicts, and hide service failures behind inconsistent ownership.

The current election helper chooses the highest-priority healthy candidate known to the local
process. That result is advisory only. It must not be treated as permission to move the VIP until
runtime heartbeat, health, and safety checks exist.

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

## Quorum

`election.require_quorum` is reserved for future behavior. It currently does not enforce quorum.

When quorum support is implemented, it should prevent a node from claiming the VIP unless the node
can observe enough healthy peers to safely make an ownership decision. The exact quorum rule should
be documented before implementation, including how two-node clusters behave and how maintenance
windows are handled.

Until quorum is implemented, real VIP movement should remain disabled by default and require an
explicit operator decision.

## Fencing and Operator Safeguards

Quorum alone may not be enough in every deployment. Future designs should consider fencing or other
operator-provided safeguards for environments where a node can become isolated but still serve
traffic.

Any future remote API that can affect ownership must require authentication, be explicitly enabled,
and avoid unauthenticated remote write access. Privileged actions should not be exposed on a network
listener by default.

## Current Status

Current behavior is intentionally non-mutating:

- election and failover decision helpers are pure local logic;
- Linux VIP manager methods build `iproute2` and `arping` command requests through a dry-run
  command runner by default;
- `--dry-run` does not change system state;
- heartbeat networking, runtime health state, quorum, fencing, and daemon lifecycle are not
  implemented yet.
