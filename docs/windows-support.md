# Windows 11 and Windows Server 2025 Support

This document is an investigation/spike for GitHub issue #107. It records how easy-failover would
run on Windows 11 and Windows Server 2025, and how a future Windows platform backend would mirror the
existing Linux backend.

**This is a spike. It contains no Windows runtime code.** easy-failover is currently a Linux-only
agent: the only platform backend lives under `src/platform/linux/`, and the build, CI, packaging, and
service integration target Linux. Nothing in this document changes runtime behavior. It exists to
scope the work and surface blockers before any Windows code is written.

For background on the existing Linux model, see [`linux-capabilities.md`](linux-capabilities.md) and
[`failover-safety.md`](failover-safety.md). The config schema is unchanged across platforms and is
documented in [`config-reference.md`](config-reference.md).

## How the Linux Backend Works Today

The current backend is intentionally simple so the Windows equivalent can mirror it. The relevant
abstractions live in `src/platform/`:

- `NetworkCommandRunner` (`src/platform/NetworkCommandRunner.hpp`) runs a `NetworkCommandRequest`
  (an `executable`, an `arguments` vector, and a `dry_run` flag) and returns a
  `NetworkCommandResult`. `LinuxNetworkCommandRunner` implements this with `fork`/`execvp`.
- `VipManager` (`src/platform/VipManager.hpp`) exposes `addVip`, `removeVip`, and `announceVip`.
  `LinuxVipManager` builds the command requests for each `VipOperationType` and runs them through a
  `NetworkCommandRunner`.
- `LinuxVipOwnershipProbe` runs a read-only query (`ip addr show dev <iface>`) and parses the output
  to decide whether this node already owns the VIP.
- `HealthCommandRunner` (`src/health/HealthCheck.hpp`) runs the configured health command with a
  timeout. `LinuxHealthCommandRunner` shells out via `/bin/sh -c`.

On Linux the VIP operations map to:

```sh
# add
ip addr add 10.0.0.50/24 dev eth0
# remove
ip addr del 10.0.0.50/24 dev eth0
# announce (gratuitous ARP), sent multiple times in two forms
arping -A -c 10 -I eth0 10.0.0.50
arping -U -c 10 -I eth0 10.0.0.50
```

All VIP operations are guarded by two controls: runtime dry-run mode and
`mutation_safety.allow_network_mutation`. A real command request is only built when the runtime is not
in dry-run mode **and** mutation is explicitly allowed (`LinuxVipManager::runOperation` computes
`effective_dry_run = dry_run || !allow_network_mutation`). A Windows backend must preserve these same
guards.

## Windows VIP Ownership Options

Windows has two practical command surfaces for assigning and removing an address on an interface:
`netsh` and the `Net*IPAddress` PowerShell cmdlets. Either fits the existing
`NetworkCommandRunner`/`VipManager` shape because both are ordinary executables invoked with an
argument vector.

### Add the VIP

Using `netsh` (no shell parsing, maps cleanly onto an `executable` + `arguments` request):

```text
netsh interface ip add address "Ethernet0" 10.0.0.50 255.255.255.0
```

Using PowerShell:

```text
New-NetIPAddress -InterfaceAlias "Ethernet0" -IPAddress 10.0.0.50 -PrefixLength 24
```

Notes:

- Windows identifies interfaces by alias (for example `Ethernet0`) or interface index, not by a Linux
  device name such as `eth0`. The `vip.interface` config field stays a plain string; on Windows it
  holds the interface alias. No config schema change is required (see
  [`config-reference.md`](config-reference.md)).
- `netsh` takes a dotted subnet mask (`255.255.255.0`); the cmdlet takes a prefix length (`24`). A
  Windows backend would convert the CIDR prefix from `vip.address` to whichever form it uses, the way
  `LinuxVipManager` already strips the prefix for `arping` via `addressWithoutPrefix`.
- A secondary IP added this way is removed automatically on reboot unless persisted; for a failover
  VIP that transient behavior is acceptable and arguably desirable.

### Remove the VIP

Using `netsh`:

```text
netsh interface ip delete address "Ethernet0" 10.0.0.50
```

Using PowerShell:

```text
Remove-NetIPAddress -IPAddress 10.0.0.50 -InterfaceAlias "Ethernet0" -Confirm:$false
```

`-Confirm:$false` is required so the cmdlet does not block on an interactive prompt under a service.

### Gratuitous ARP / Neighbor Refresh

This is the most important behavioral difference from Linux.

- **Windows sends a gratuitous ARP automatically when an IPv4 address is added to an interface.** The
  TCP/IP stack announces the new address as part of duplicate-address detection and address
  assignment. There is therefore no separate "announce" step required to seed neighbor caches the way
  Linux uses `arping -A`/`-U`.
- **`arping` is not native to Windows.** There is no built-in equivalent that resends gratuitous ARP
  on demand. Third-party tools exist but should not be a runtime dependency.
- For IPv6, neighbor discovery (unsolicited neighbor advertisements) is likewise handled by the
  stack on address assignment.

Implications for a Windows `VipManager`:

- `addVip` already triggers the gratuitous ARP as a side effect of `New-NetIPAddress` / `netsh ... add
  address`, so the Windows `announceVip` can reasonably be a no-op (return a successful, empty result)
  or a best-effort refresh.
- If extra reassurance is wanted after a flapping failover, the backend could re-trigger announcement
  by removing and re-adding the address, or by clearing the relevant neighbor entries with
  `netsh interface ip delete neighbors "<alias>"` so peers re-resolve. Both are optional refinements,
  not requirements, and should be validated against the target switching and hypervisor environment
  exactly as the Linux notes in [`failover-safety.md`](failover-safety.md) recommend.

### Mapping onto the Existing Abstraction

A future `WindowsVipManager` would mirror `LinuxVipManager` one-to-one:

| `VipOperationType` | Linux | Windows (proposed) |
| --- | --- | --- |
| `Add` | `ip addr add <addr> dev <iface>` | `netsh interface ip add address <alias> <ip> <mask>` |
| `Remove` | `ip addr del <addr> dev <iface>` | `netsh interface ip delete address <alias> <ip>` |
| `Announce` | `arping -A`/`-U` | no-op (gratuitous ARP is automatic on `Add`) |

A `WindowsVipOwnershipProbe` would mirror `LinuxVipOwnershipProbe` by running a read-only query such
as `netsh interface ip show address "<alias>"` (or `Get-NetIPAddress -InterfaceAlias "<alias>"`) and
checking whether the configured address appears in the output. The same dry-run guard and
`allow_network_mutation` checks must apply.

## Service Integration

On Linux the daemon runs under systemd (`packaging/systemd/easy-failover.service.in`) with
`--run-forever`. On Windows the equivalent is a service under the Service Control Manager (SCM). The
same applies to both **Windows 11** and **Windows Server 2025**; Server 2025 is the more likely
production target, while Windows 11 is mainly useful for development and lab validation.

Options for running easy-failover as a Windows Service:

- **Native service support in the binary.** The cleanest long-term option is to teach the daemon to
  speak the SCM service-control protocol (register a service control handler, report
  `SERVICE_RUNNING`/`SERVICE_STOPPED`, and handle stop/shutdown control codes the way the Linux build
  handles `SIGINT`/`SIGTERM`). The existing shutdown-request model maps directly onto an SCM stop
  control.
- **`sc.exe` to register an existing executable.** A service can be created without code changes:

  ```text
  sc.exe create easy-failover binPath= "C:\Program Files\easy-failover\easy-failover.exe --config C:\ProgramData\easy-failover\config.toml --run-forever" start= auto
  sc.exe description easy-failover "easy-failover virtual IP failover agent"
  sc.exe start easy-failover
  ```

  Without native SCM integration the process must still respond correctly to a service stop. A plain
  console executable will be terminated rather than asked to shut down cleanly, so this is acceptable
  for a first spike but not for production.
- **A service wrapper** such as WinSW or NSSM can supervise the console binary and translate SCM
  control codes into process signals/termination. This avoids Windows-specific code at the cost of an
  extra dependency in the packaging story.

Recommended path: native SCM integration in a Windows build, with `sc.exe`/wrapper supervision as the
interim approach for early lab testing.

## Health Command Support

The daemon runs a single configured health command (`health.command`) with a timeout
(`health.timeout_ms`, default 2000 ms) and an interval (`health.interval_ms`). On Linux,
`LinuxHealthCommandRunner` executes it via `/bin/sh -c "<command>"` and enforces the timeout itself,
killing the process group when the deadline passes. The timeout bounds the entire run, including
process startup.

For Windows:

- There is no `/bin/sh`. A Windows health runner would shell out via `cmd.exe /c "<command>"` for
  classic command lines, or `powershell.exe -NoProfile -Command "<command>"` (or `pwsh`) for
  PowerShell. The sample config command `curl -fsS http://127.0.0.1:8080/health` works under modern
  Windows because `curl.exe` ships with Windows 10/11 and Server 2019+, but the quoting and exit-code
  conventions differ from POSIX shells.
- Exit-code convention is the same in spirit: `0` means healthy, non-zero means unhealthy. PowerShell
  callers must be careful that `$LASTEXITCODE` / `exit` is propagated, since a cmdlet that throws does
  not always yield a non-zero process exit code by default.
- The timeout model carries over conceptually, but the implementation must change. The Linux runner
  relies on `fork`, `setpgid`, process-group signals, and `SIGTERM`/`SIGKILL`. A Windows runner would
  use `CreateProcess` with a Job Object (so child trees can be terminated together) and
  `TerminateJobObject` on timeout, reporting the same `CommandResult { exit_code, timed_out, error }`
  shape that `HealthCommandRunner` already defines.
- Health-check command execution is currently behind an internal runner and not yet wired into the
  decision loop on any platform (see [`config-reference.md`](config-reference.md)), so this is
  forward-looking work that should land alongside the Linux integration rather than ahead of it.

## Firewall Requirements

The daemon binds a UDP heartbeat socket on `heartbeat.bind`, default `0.0.0.0:7432`, and sends
datagrams to each peer's `host:port` (see [`config-reference.md`](config-reference.md)). On Windows,
Windows Defender Firewall blocks unsolicited inbound UDP by default, so peers will not receive each
other's heartbeats until rules are added.

Required rules (adjust the port if `heartbeat.bind` is changed):

```text
netsh advfirewall firewall add rule name="easy-failover heartbeat in" dir=in action=allow protocol=UDP localport=7432
netsh advfirewall firewall add rule name="easy-failover heartbeat out" dir=out action=allow protocol=UDP localport=7432
```

Or with PowerShell:

```text
New-NetFirewallRule -DisplayName "easy-failover heartbeat in"  -Direction Inbound  -Protocol UDP -LocalPort 7432 -Action Allow
New-NetFirewallRule -DisplayName "easy-failover heartbeat out" -Direction Outbound -Protocol UDP -LocalPort 7432 -Action Allow
```

Notes:

- The outbound rule is usually not required because Windows allows outbound traffic by default, but it
  is listed for environments with a restrictive outbound policy.
- If the optional local API is ever enabled on Windows, it should stay bound to loopback
  (`127.0.0.1`, default) and should not get a firewall exception. The API is read-only and disabled by
  default; see [`local-api-design.md`](local-api-design.md).
- Packaging should offer to create the heartbeat rule but must not silently open the firewall without
  operator intent, consistent with the project's opt-in safety posture.

## Required Privileges

On Linux, real VIP movement needs `CAP_NET_ADMIN` (interface address changes) and `CAP_NET_RAW`
(`arping` raw packets); see [`linux-capabilities.md`](linux-capabilities.md). The Windows equivalents:

- **Administrator rights are required to add or remove an interface IP address.** `New-NetIPAddress`,
  `Remove-NetIPAddress`, and `netsh interface ip ...` all require an elevated token. Running as a
  Windows Service typically means running as `LocalSystem`, which has the necessary privileges, or as
  a dedicated service account granted the required rights.
- Because Windows sends gratuitous ARP from the stack on address add, there is **no raw-socket
  requirement** analogous to `CAP_NET_RAW` for the announce step. This removes one of the two Linux
  privilege needs.
- Preferred posture mirrors the Linux guidance: run with the least privilege that works. `LocalSystem`
  is the simplest starting point but is broad; a dedicated service account with only the rights needed
  to modify the target interface is the better long-term model. Windows does not have a single
  capability as narrow as `CAP_NET_ADMIN`, so least-privilege here is coarser and must be validated on
  the target OS.
- The opt-in safety controls still apply: even with Administrator rights, no real mutation happens
  unless the runtime is not in dry-run mode and `mutation_safety.allow_network_mutation = true`, and
  the daemon still waits through one successful heartbeat warmup cycle (see
  [`failover-safety.md`](failover-safety.md)).

## Blockers and Follow-up Implementation Tasks

Concrete next steps to turn this spike into real support:

1. **Add a Windows platform backend under `src/platform/windows/`** mirroring the Linux one:
   - `WindowsNetworkCommandRunner` implementing `NetworkCommandRunner` with `CreateProcess` (instead
     of `fork`/`execvp`), capturing stdout/stderr and exit code into `NetworkCommandResult`.
   - `WindowsVipManager` implementing `VipManager` with the `netsh`/`Net*IPAddress` command mapping
     above, preserving the `dry_run || !allow_network_mutation` guard.
   - `WindowsVipOwnershipProbe` running a read-only address query and parsing it.
   - `WindowsHealthCommandRunner` using `cmd.exe`/`powershell.exe` with Job Object timeout
     enforcement, returning the existing `CommandResult` shape.
   - A `WindowsHostname` provider for the `node_id` hostname default.
2. **Build system.** Make `CMakeLists.txt` select the platform backend by target OS and build a
   Windows binary (MSVC and/or clang-cl). The project targets C++23, so the chosen toolchain must
   support it. Resolve any POSIX-only assumptions in shared code.
3. **Service integration.** Decide between native SCM integration and a wrapper, then implement and
   document the chosen path for Windows 11 and Windows Server 2025.
4. **Packaging.** Define a Windows install layout (binary, example config under `C:\ProgramData`,
   service registration) analogous to the Linux install rules, and a way to ship it (for example an
   MSI or a zip plus an install script).
5. **CI.** Add a Windows runner to GitHub Actions that builds the project, runs CTest, and runs the
   same smoke checks (`--version`, config validation, dry-run) the Linux job runs.
6. **Firewall and privilege automation.** Provide optional, opt-in helpers to create the heartbeat
   firewall rule and to register the service with appropriate rights.

Known blockers and open questions:

- **Build toolchain and shared-code portability.** Substantial parts of the existing platform code use
  POSIX APIs (`fork`, `pipe`, `poll`, `waitpid`, signals). These have no direct Windows equivalent and
  must be reimplemented per platform; only the abstraction interfaces are portable.
- **Interface identity.** Windows interface aliases/indices differ from Linux device names. Operators
  must supply a valid Windows alias in `vip.interface`; documentation and validation should make this
  clear (the schema itself does not change).
- **No native `arping`.** Acceptable because the stack announces on add, but the convergence behavior
  must be validated on real Windows networks, hypervisors, and switches, exactly as the Linux notes
  caution.
- **Quorum and fencing are still unimplemented on every platform.** Windows support does not change the
  split-brain safety story; real VIP movement remains opt-in and lab-only until the safety work in
  [`failover-safety.md`](failover-safety.md) lands.
- **Testing environment.** Validation needs real Windows 11 and Windows Server 2025 hosts (or VMs) with
  two-node networking; CI alone will not exercise real VIP movement.

Until these are addressed, easy-failover remains Linux-only. Again: **this document is a spike and
includes no Windows runtime code.**
