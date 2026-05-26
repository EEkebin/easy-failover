# Linux Capability Notes

easy-failover does not currently perform real VIP movement. The Linux VIP backend builds
non-mutating `iproute2` and `arping` command requests by default, and real mutation remains blocked
behind explicit safety controls.

This document records the expected Linux privileges for the future point where real VIP movement is
enabled.

## Required Tools

The first Linux VIP backend shells out through `NetworkCommandRunner`. A host that enables real VIP
movement will need:

- `ip` from `iproute2` for `ip addr add` and `ip addr del`;
- `arping` for IPv4 gratuitous ARP announcements;
- a service environment where those binaries are available through an explicit path or a controlled
  `PATH`.

Package names vary by distribution. Common examples are `iproute2` for `ip` and either `iputils` or
`iputils-arping` for `arping`.

## Expected Capabilities

Real VIP movement should use the smallest practical privilege set.

- `CAP_NET_ADMIN`: needed for network interface address changes such as adding or removing a VIP
  with `ip addr`.
- `CAP_NET_RAW`: commonly needed for ARP announcement tools that create raw packets, including
  `arping`.

Running the service as `root` can satisfy these requirements, but it is broader than the target
operating model. The preferred future deployment model is a dedicated service account with only the
capabilities required for the selected backend and packet announcement method.

## systemd Guidance

The packaged unit intentionally still runs as `root` and only contains hardening TODOs. Do not grant
or tighten capabilities in the unit until runtime ownership logic is wired and tested.

When real VIP movement is enabled, the unit should be revisited with settings such as:

```ini
CapabilityBoundingSet=CAP_NET_ADMIN CAP_NET_RAW
AmbientCapabilities=CAP_NET_ADMIN CAP_NET_RAW
NoNewPrivileges=true
```

Those settings must be validated with the exact command backend, distribution packages, and service
user. Some `arping` builds may use file capabilities or setuid behavior differently, so the final
packaging pass should test the actual target distributions rather than assuming all builds behave
the same way.

## Safety Requirements

Capabilities are not a substitute for failover safety. Before real mutation is enabled, the runtime
must still enforce:

- dry-run support for every VIP operation;
- explicit operator opt-in for real network mutation;
- fresh peer heartbeat state;
- local health-check state;
- split-brain and quorum handling appropriate for the deployment.

The current CLI dry-run path must remain non-mutating.
