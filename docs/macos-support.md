# macOS Support Investigation

This document is an investigation spike for GitHub issue #108 ("Investigate macOS
support"). It records how a future macOS backend for easy-failover could mirror the existing
Linux backend, what it would shell out to, what privileges it would need, and which blockers
remain.

**No macOS runtime changes are included in this spike.** Nothing here adds, modifies, or wires up
a macOS code path. macOS remains a future planned target alongside Windows, as noted in the
project README. The current daemon still builds and runs only the Linux VIP backend, and real VIP
movement stays guarded by runtime dry-run mode and
`mutation_safety.allow_network_mutation`.

## How the Linux Backend Works Today

The Linux backend never talks to the kernel directly for VIP movement. It builds command requests
and runs them through a `NetworkCommandRunner` abstraction, so dry-run and real execution share one
path. A future macOS backend should reuse the same abstractions rather than introducing a parallel
one.

The relevant interfaces live under `src/platform/`:

- `NetworkCommandRunner` (`src/platform/NetworkCommandRunner.hpp`): runs a
  `NetworkCommandRequest` (`executable`, `arguments`, `dry_run`) and returns a
  `NetworkCommandResult`. `DryRunNetworkCommandRunner` records the request without executing it.
- `VipManager` (`src/platform/VipManager.hpp`): `addVip`, `removeVip`, and `announceVip`. Each
  returns a `VipOperationResult` that carries every `NetworkCommandResult` it ran.
- `VipOwnershipProbe` (`src/platform/VipOwnershipProbe.hpp`): `probeLocalVip` reports whether the
  local host already owns the VIP on the configured interface.

The Linux implementations show the pattern a macOS backend should follow:

- `LinuxNetworkCommandRunner` forks, `execvp`s the requested binary, captures stdout/stderr, and
  maps the wait status to an exit code. This is plain POSIX `fork`/`exec`/`waitpid`, so it is
  already portable to macOS with no logic change.
- `LinuxVipManager::buildLinuxVipCommands` maps each `VipOperationType` onto concrete commands:
  - `Add`: `ip addr add <address> dev <interface>`
  - `Remove`: `ip addr del <address> dev <interface>`
  - `Announce`: `arping -A -c 10 -I <interface> <ip>` and `arping -U -c 10 -I <interface> <ip>`
- `LinuxVipOwnershipProbe` runs `ip addr show dev <interface>` and scans the output for an `inet`
  line matching the configured address.

Real execution is gated: `LinuxVipManager::runOperation` computes
`effective_dry_run = dry_run || !allow_network_mutation`, so commands stay dry-run unless an
operator has opted in and the runtime is not in `--dry-run`. A macOS backend must preserve this
gate exactly. See [`failover-safety.md`](failover-safety.md) for the full safety model.

## macOS VIP Ownership Options

macOS is a BSD-derived system. It does not ship `iproute2`, and its `ifconfig` follows BSD syntax,
not the Linux `ip` syntax. A macOS `VipManager` would build BSD `ifconfig` commands behind the same
`NetworkCommandRunner` abstraction.

### Adding and removing a VIP

On macOS, a secondary ("alias") IPv4 address is added and removed with `ifconfig`:

- Add: `ifconfig <interface> alias <ip> netmask <netmask>`
  (for example `ifconfig en0 alias 10.0.0.50 netmask 255.255.255.0`)
- Remove: `ifconfig <interface> -alias <ip>`
  (for example `ifconfig en0 -alias 10.0.0.50`)

Two address-format differences matter when mapping the existing config:

- BSD `ifconfig` takes a dotted-quad netmask (or a `0x`-prefixed hex mask), not a CIDR prefix
  length. The Linux backend passes the CIDR address straight through to `ip addr add`. A macOS
  backend must split the configured `vip.address` (for example `10.0.0.50/24`) into a bare address
  plus a converted netmask. The existing `addressWithoutPrefix` helper in `LinuxVipManager.cpp`
  already strips the prefix; the macOS backend would additionally translate the prefix length to a
  dotted-quad or hex mask.
- Interface names differ. Linux configs typically use `eth0`; macOS uses `en0`, `en1`, and so on.
  This is just config data (`vip.interface`), so no schema change is required, but example configs
  and docs should call out the platform-appropriate name.

### Gratuitous ARP equivalent

The Linux backend uses `arping -A` (ARP announcement) and `arping -U` (unsolicited ARP update) to
refresh client neighbor caches after a VIP move. `arping` is **not** a standard macOS tool, so the
`Announce` operation needs a different approach. Options, roughly in order of preference:

- **Rely on the implicit announcement.** When macOS brings up an `ifconfig ... alias`, the kernel
  typically emits a gratuitous ARP for the new address as part of address configuration. For many
  networks this is sufficient, and the macOS `Announce` operation could be a documented no-op (or a
  thin verification step) rather than a separate ARP tool invocation. This must be validated
  against the target switches, clients, and hypervisors before being relied on, exactly as the
  Linux notes already caution.
- **Use the BSD neighbor tools to prime/inspect caches.** `arp` (IPv4) and `ndp` (IPv6 neighbor
  discovery) are present on macOS and can manipulate or flush neighbor entries, but they do not
  send broadcast gratuitous ARP the way `arping -A`/`-U` does. They are useful for the
  `VipOwnershipProbe` side and for diagnostics more than for proactive announcement.
- **Ship or build a small announcer.** A future backend could bundle a raw-socket gratuitous-ARP
  sender (a native helper, or a third-party tool installed via the chosen packaging path). This is
  the closest behavioral match to the Linux `arping` forms but adds a packaging and codesigning
  dependency (see Required Privileges below).

For the ownership probe, the macOS equivalent of `ip addr show dev <interface>` is
`ifconfig <interface>`. The existing `outputContainsVip` scanner in `LinuxVipOwnershipProbe.cpp`
already keys on `inet <address>` tokens, and BSD `ifconfig` output also prints `inet <address>`
lines, so the same parsing strategy should port with little change.

### BSD `ifconfig` vs Linux `iproute2` summary

| Operation        | Linux (`iproute2`)                         | macOS (BSD `ifconfig`)                              |
| ---------------- | ------------------------------------------ | -------------------------------------------------- |
| Add VIP          | `ip addr add 10.0.0.50/24 dev eth0`        | `ifconfig en0 alias 10.0.0.50 netmask 255.255.255.0` |
| Remove VIP       | `ip addr del 10.0.0.50/24 dev eth0`        | `ifconfig en0 -alias 10.0.0.50`                    |
| Gratuitous ARP   | `arping -A` / `arping -U`                  | implicit on alias add; no standard `arping`        |
| Inspect / probe  | `ip addr show dev eth0`                    | `ifconfig en0`                                     |
| Address format   | CIDR prefix (`/24`)                        | dotted-quad or hex netmask                         |

A `MacosVipManager` and `MacosVipOwnershipProbe` mirroring the Linux classes, plus a reusable
POSIX command runner, would let the rest of the daemon stay platform-agnostic.

## launchd Integration

macOS has no systemd. The supervisor is **launchd**, configured with a property-list (`.plist`)
file. A system-level failover daemon belongs in `/Library/LaunchDaemons/` (runs as root, started at
boot, independent of any logged-in user), not `/Library/LaunchAgents/` (per-user, GUI sessions).

The launchd daemon is the analog of the existing systemd unit
(`packaging/systemd/easy-failover.service.in`). It should run the same `--run-forever` invocation
against the installed config. A first-cut `com.easy-failover.daemon.plist` would look like:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.easy-failover.daemon</string>
    <key>ProgramArguments</key>
    <array>
        <string>/usr/local/sbin/easy-failover</string>
        <string>--config</string>
        <string>/usr/local/etc/easy-failover/config.toml</string>
        <string>--run-forever</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>/var/log/easy-failover/easy-failover.log</string>
    <key>StandardErrorPath</key>
    <string>/var/log/easy-failover/easy-failover.log</string>
</dict>
</plist>
```

Notes mapping launchd onto the systemd unit:

- `RunAtLoad` is the rough analog of `WantedBy=multi-user.target` plus start-on-boot.
- `KeepAlive` is the analog of `Restart=on-failure`. `KeepAlive` can also be a dictionary that only
  restarts on nonzero exit, which is closer to `on-failure`.
- launchd has no direct equivalent of the systemd sandbox directives
  (`ProtectSystem`, `PrivateTmp`, capability bounding, and so on). macOS confinement is done through
  a different mechanism (codesigning, entitlements, and the platform sandbox), so the hardening
  story does not port one-to-one and is a follow-up design question.
- Daemons load with `launchctl bootstrap system /Library/LaunchDaemons/com.easy-failover.daemon.plist`
  (modern) or `launchctl load` (legacy), analogous to `systemctl daemon-reload` plus
  `systemctl enable --now`.

As with the systemd unit, the daemon should ship installed but not auto-enabled, and
`mutation_safety.allow_network_mutation` should stay `false` until real VIP movement has been
validated in the target environment.

## Health Command Support

Health-check command execution is already written against POSIX primitives and is largely
portable. `LinuxHealthCommandRunner` (`src/platform/linux/LinuxHealthCommandRunner.cpp`):

- executes the configured command with `execl("/bin/sh", "sh", "-c", command, ...)`;
- enforces `health.timeout_ms` with a `steady_clock` deadline driven by `poll`/`waitpid`;
- runs the command in its own process group (`setpgid`) and, on timeout, terminates the whole
  group with `SIGTERM` then `SIGKILL`.

All of those calls (`fork`, `execl`, `setpgid`, `poll`, `waitpid`, `kill(-pgid, ...)`) exist on
macOS with the same semantics, so the timeout-and-cleanup model should port directly. The runner is
also not yet wired into the daemon loop on any platform (see
[`config-reference.md`](config-reference.md)), so this is forward-looking either way.

Shell differences to account for:

- The health command runs under `/bin/sh`, which on macOS is provided by `bash` in POSIX mode
  (older releases) and is still present on current releases. easy-failover invokes `/bin/sh`
  directly rather than the user's interactive shell, so the macOS default interactive shell being
  `zsh` does not affect health-command execution.
- Config authors should keep health commands POSIX `sh`-compatible and not assume `bash`-only or
  `zsh`-only syntax, since the same `command` string is meant to run on both Linux and macOS.
- Tooling differs: the sample `curl -fsS http://127.0.0.1:8080/health` works on macOS as-is, but
  any Linux-specific binaries in a custom health command would need macOS equivalents.

## Required Privileges

macOS does **not** have the Linux capabilities model. There is no `CAP_NET_ADMIN` or
`CAP_NET_RAW`; the corresponding privileged operations require running as **root** (uid 0). See
[`linux-capabilities.md`](linux-capabilities.md) for the Linux model this contrasts with.

- `ifconfig <interface> alias`/`-alias` changes require root.
- Raw-socket / ARP operations (any bundled gratuitous-ARP sender) require root for raw `AF_NDRV` or
  `BPF` access; there is no per-binary capability to narrow this to.
- Reading state with `ifconfig <interface>` does not need root, so the `VipOwnershipProbe` path can
  run unprivileged even though mutation cannot.

Because there is no capability bounding set, the macOS deployment is effectively root-or-nothing for
VIP mutation. That is broader than the Linux target operating model (dedicated account plus minimal
capabilities), and it should be called out clearly in any macOS deployment guidance.

Additional macOS-only considerations that have no Linux analog:

- **Codesigning.** macOS expects daemons and any bundled helpers to be codesigned (and, for
  distribution outside a controlled fleet, notarized). Unsigned binaries trigger Gatekeeper and
  quarantine prompts.
- **Entitlements.** If a future native announcer uses restricted networking interfaces, it may need
  specific entitlements baked in at signing time.
- **System Integrity Protection (SIP) and TCC.** SIP restricts where system daemons may live and
  what they may modify; install paths and the launchd plist location must respect it.

## Blockers and Follow-up Implementation Tasks

This spike confirms the architecture supports a macOS backend without disturbing the platform
abstractions, but several concrete pieces of work and open questions remain.

### Follow-up implementation tasks

- **`src/platform/macos/` backend.** Add `MacosVipManager`, `MacosVipOwnershipProbe`, and reuse (or
  generalize) the POSIX command runner. Mirror the Linux classes:
  - map `Add`/`Remove`/`Announce` onto `ifconfig alias`/`-alias` and the chosen announce strategy;
  - add CIDR-prefix-to-netmask conversion (dotted-quad or hex) for `ifconfig`;
  - reuse the `effective_dry_run = dry_run || !allow_network_mutation` gate unchanged.
- **Build wiring.** Add macOS source selection in `CMakeLists.txt` and a platform switch so the
  daemon selects the macOS backend on Apple targets. (No build files are changed in this spike.)
- **Packaging.** Decide the macOS install layout (paths under `/usr/local` or a signed package),
  ship a `com.easy-failover.daemon.plist` template analogous to the systemd unit, and document
  `launchctl bootstrap`/`bootout` instead of `systemctl`.
- **CI runner.** Add a macOS GitHub Actions runner that builds the project, runs CTest, and runs the
  `--version` / config-validation / dry-run smoke checks, mirroring the Linux CI job.
- **Docs.** Extend the install and packaging docs with macOS paths, privileges, and the launchd
  workflow once the backend exists.

### Known blockers and open questions

- **Gratuitous ARP gap.** There is no drop-in `arping` replacement on macOS. The
  rely-on-implicit-announcement approach must be validated against real switches/clients/hypervisors
  before the `Announce` operation can be trusted, and a bundled announcer adds codesigning and
  raw-socket-privilege work.
- **Hardening parity.** The systemd unit's sandbox directives have no direct launchd equivalent;
  achieving comparable confinement on macOS (codesigning, entitlements, the platform sandbox) is an
  open design task, not a translation.
- **Codesigning/notarization pipeline.** Distributing a signed, notarized daemon requires an Apple
  Developer identity and signing infrastructure in CI that the project does not have today.
- **No macOS test environment yet.** Until a macOS CI runner and a two-node macOS lab exist, the
  backend cannot be exercised end-to-end; all VIP behavior must be validated in a lab before any
  production use, consistent with [`failover-safety.md`](failover-safety.md).
- **Safety model unchanged.** macOS support does not change the outstanding quorum/fencing work.
  Real VIP movement must remain disabled by default and opt-in on macOS exactly as on Linux.

## Status

This is an investigation spike only. It introduces no macOS runtime code, no build changes, and no
packaging. It exists to scope the work and record the design so a future change can implement a
macOS backend that mirrors the Linux one behind the existing `NetworkCommandRunner` / `VipManager`
abstractions.
