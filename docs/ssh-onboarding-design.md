# Server-Side SSH Onboarding Design

easy-failover nodes are installed by hand today: an operator builds from source or unpacks a
release tarball, writes `/etc/easy-failover/config.toml`, validates it, and enables the service.
This document designs an optional dashboard feature that performs that same flow over SSH for a new
node, driven from the dashboard's server. It is a design only. No onboarding code exists yet, and
nothing here changes the daemon, the installer scripts, or the existing read-only API.

The onboarding flow deliberately mirrors the source install flow: it builds
a Release binary, installs under a prefix, keeps configuration under `/etc/easy-failover`, validates
the config with `--validate-config`, and never enables real VIP mutation as part of installation.

See also: [`config-reference.md`](config-reference.md) for the TOML schema the generated config must
satisfy, [`failover-safety.md`](failover-safety.md) for the safety posture this flow must preserve,
[`frontend-roadmap.md`](frontend-roadmap.md) for the dashboard context, and the sibling
[`write-api-design.md`](write-api-design.md) for the authenticated write API this feature plugs into.

## Goals

- Let an operator onboard a new easy-failover node from the dashboard without leaving a terminal.
- Reuse the existing source install flow (the source install flow) and the
  TOML schema in [`config-reference.md`](config-reference.md) rather than inventing a parallel one.
- Keep all SSH and all secret handling on the dashboard server. Never ship credentials, key
  material, or SSH logic to the browser.
- Leave a freshly onboarded node in a safe state: installed, validated, running, but with real VIP
  mutation disabled until the operator validates it.
- Surface clear, redacted progress and clear failures, with retry that is safe to run again.

## Non-Goals

- Implementing the onboarding code. This is a design document.
- Designing the authenticated write API itself; that lives in
  [`write-api-design.md`](write-api-design.md). This document only states how onboarding rides on
  top of it.
- Cluster-wide orchestration, fleet rollout, or config drift reconciliation across existing nodes.
- Enabling or tuning real VIP movement. Onboarding stops at a validated, running, non-mutating node.

## Trust Boundary: Server-Side Only

All SSH activity runs inside the dashboard's server-side runtime: a Next.js route handler or server
action, using a server-side SSH client such as [`ssh2`](https://github.com/mscdex/ssh2). The browser
is never an SSH client and never holds a usable secret.

- The browser collects operator inputs in a form and POSTs them to a server endpoint over HTTPS.
- The server opens the SSH connection, runs every step, and streams back **redacted** progress
  (status text, step names, exit codes, redacted command lines), never raw secrets.
- Private keys, passwords, passphrases, and sudo passwords exist only in server process memory for
  the duration of the run. They are never serialized into client-bound responses, never written to
  any client component, and never embedded in the JavaScript build output.
- The onboarding endpoint must require an authenticated dashboard session and the write authority
  defined in [`write-api-design.md`](write-api-design.md). Onboarding mutates a remote host, so it is
  a privileged write action, not a read action, and must be off by default and explicitly enabled,
  consistent with [`failover-safety.md`](failover-safety.md) and
  [`frontend-roadmap.md`](frontend-roadmap.md).

Because secrets never reach client JS and are never baked into the build, a compromised browser
session cannot exfiltrate stored SSH credentials: there are none to steal (see Ephemeral
Credentials below).

## Ephemeral Credentials by Default

Onboarding secrets are ephemeral by default. The SSH password, the private-key passphrase, and the
sudo password are:

- accepted only as inputs to a single onboarding run;
- held only in server process memory, only for the duration of that run;
- never written to disk, never written to a database, never written to environment files, and never
  persisted in any cache or session store;
- dropped (overwritten/dereferenced) as soon as the run finishes or fails;
- never returned to the client in any form.

Persisting credentials for unattended re-onboarding is explicitly out of scope (see Deferred). If a
future design adds it, it must use a real secret store and remain opt-in, and that design lives
elsewhere.

### Redaction Rule

Every secret value is redacted everywhere it could otherwise appear. The redaction rule is:

> Before any string is logged, returned to the client, or recorded in run history, replace every
> occurrence of every secret the operator supplied — SSH password, private key, key passphrase, and
> sudo password — with the literal `***`. Command lines are built so secrets are passed out-of-band
> (stdin, the SSH auth layer, or `sudo -S` on stdin), never as argv that could be echoed; any
> command line that still references a secret is redacted to `***` before display.

This applies to progress events, structured logs, error messages, and stored run records. A log line
that would read `sshpass -p hunter2 ...` is never produced; the displayed form is `... ***`. The
key principle: secrets travel out-of-band and are redacted to `***` in every observable output.

## Operator Inputs

The dashboard form collects the full parameter set for one onboarding run.

### Connection

- `host`: target hostname or IP.
- `port`: SSH port (default `22`).
- `username`: SSH login user.
- One authentication method:
  - `password`: SSH password; or
  - `privateKey`: PEM/OpenSSH private key, with optional `passphrase`.
- `sudoMethod`: how the run gains root on the target:
  - `passwordless` — user has passwordless `sudo`;
  - `password` — user has `sudo` but needs a password, supplied as `sudoPassword` and fed via
    `sudo -S` on stdin;
  - `already-root` — the login user is already `root`, so no `sudo` is used.

`password`, `passphrase`, and `sudoPassword` are ephemeral secrets and follow the redaction rule.

### Install Source

The operator picks how easy-failover gets onto the target. Both map to the existing install flow.

- `release-tarball` (recommended default): download a published release tarball plus its `.sha256`
  file, verify with `sha256sum -c`, and install the contents. This matches the release artifacts
  described in [`../README.md`](../README.md) (a Linux x86_64 tarball and a SHA-256 checksum file
  published per tag) and is the fastest, most reproducible path with the fewest target prerequisites.
  - `releaseRef`: release tag (for example `v1.2.3`) or `latest`.
  - SHA-256 verification is mandatory; a checksum mismatch fails the run before anything is installed.
- `build-from-source`: clone the repository at a git ref and run
  the source install flow on the target.
  - `gitRef`: branch, tag, or commit to build (defaults to the default branch).
  - This path needs a full build toolchain on the target (a C++23 compiler, CMake, a build tool,
    Git) as listed in [`../README.md`](../README.md), so prerequisite installation does more work.

**Recommended default: `release-tarball`.** It is reproducible, needs no compiler on the target, and
its integrity is checked by SHA-256. Build-from-source is offered for nodes pinned to a specific
unreleased ref or architecture without a published tarball.

Common install knobs for both paths map to installer flags:

- `prefix` (default `/usr`) → `--prefix`.
- `sysconfdir` (default `/etc`) → `--sysconfdir`; config lands at `${sysconfdir}/easy-failover/config.toml`.

### Generated easy-failover Config

The operator does not paste raw TOML. The dashboard collects the fields from
[`config-reference.md`](config-reference.md) and renders a `config.toml` on the server:

- `node_id` (defaults to the target hostname if blank) and `priority`;
- `[vip].address` and `[vip].interface`;
- `[[peers]]` entries (`id`, `address`) — at least one is required;
- optional `[heartbeat]`, `[health]`, `[election]`, `[api]` overrides;
- `[mutation_safety].allow_network_mutation` is forced to `false` for onboarding (see Safety).

The rendered config is written to `${sysconfdir}/easy-failover/config.toml` on the target and then
validated in place with `easy-failover --config <path> --validate-config` before the service is
enabled, exactly as the source install flow does. The dashboard should also
validate inputs against the schema client-side and server-side for fast feedback, but the
authoritative check is the on-target `--validate-config` run.

## Onboarding Step Sequence

The run is an ordered sequence of steps. Each step reports a status (`pending`, `running`, `ok`,
`failed`, `skipped`), a short human label, redacted detail, and any captured exit code. Progress
streams to the browser as redacted events. A failure stops the sequence (subject to per-step retry,
below) and leaves the run in a known state.

1. **Connect** — open the SSH session with the chosen auth method; confirm reachability and that the
   login user resolves. Reports: connected host/port/user, server banner. Redacts all auth material.
2. **Detect distro and prereqs** — read `/etc/os-release`, detect architecture, detect the init
   system, and check for required tools. Reports: distro/version, arch, detected service manager,
   which prerequisites are present vs missing. No changes yet.
3. **Install prereqs** — install missing prerequisites with the detected package manager (for
   example `apt`, `dnf`, `pacman`) under the chosen sudo method. For `release-tarball` this is
   minimal (`tar`, `sha256sum`, `iproute2`/`arping` for hosts that will move a real VIP); for
   `build-from-source` it includes the full toolchain from [`../README.md`](../README.md). Reports:
   packages installed, manager used.
4. **Fetch / build / install easy-failover** — for `release-tarball`: download the tarball and its
   `.sha256`, run `sha256sum -c` (fail on mismatch), unpack, and install into `prefix`/`sysconfdir`.
   For `build-from-source`: clone at `gitRef` and run
   the source install flow with the mapped `--prefix`/`--sysconfdir`, which
   builds Release and installs without enabling the service. Reports: source, ref, verified checksum
   or build result, installed binary path.
5. **Write config** — write the server-rendered `config.toml` to
   `${sysconfdir}/easy-failover/config.toml` with `0644`, creating the directory `0755`. If a config
   already exists, do not silently overwrite a divergent one (see Idempotency). Reports: config path
   and a redacted summary (vip, peer count, node_id), never secrets.
6. **Validate config** — run `easy-failover --config <path> --validate-config` on the target. A
   non-zero exit fails the run before the service is enabled. Reports: validation pass/fail and the
   validator's error output.
7. **Enable and start the service** — using the **target's** init system (see below), install/enable
   the unit and start it. The daemon starts with `allow_network_mutation = false`, so it runs without
   moving a real VIP. Reports: service manager, unit name, enabled/started result.
8. **Verify** — confirm the service is active and, if `[api].enabled = true` was configured, poll the
   node's read-only API for a status snapshot and confirm heartbeat activity. Reports: service active
   state, API reachability, observed heartbeat/peer state. This is a verification step, not a VIP
   ownership change.

### Service Manager Selection

easy-failover targets five init systems: **systemd, OpenRC, runit, dinit, and s6**. Step 2 detects
which one the target runs (for example, `systemd` via `systemctl`/`/run/systemd/system`, OpenRC via
`rc-status`/`/etc/init.d`, and the supervision suites by their service directories), and step 7 uses
the matching service manager:

- systemd: install the `easy-failover.service` unit, `daemon-reload`, then `enable --now`.
- OpenRC: install the init script, `rc-update add`, then `rc-service ... start`.
- runit / s6 / dinit: install the service definition into the supervision tree and start it the way
  that supervisor expects.

The installer scripts themselves only handle systemd reloads
(the source install flow skips the reload when `systemctl` is absent), so
non-systemd enablement is an onboarding-side responsibility layered on top of the install. The design
should pick the service manager from detection, not assume systemd.

## Safety Posture

Onboarding never enables real VIP mutation. This preserves the project's safety model from
[`failover-safety.md`](failover-safety.md) and [`config-reference.md`](config-reference.md):

- The generated config sets `[mutation_safety].allow_network_mutation = false`. The node installs,
  validates, and runs, but every VIP operation stays in dry-run command form.
- Turning on real mutation is a separate, explicit operator decision made after the node has been
  validated in the target environment. Onboarding must not flip that flag, and the dashboard must
  not offer to flip it as part of onboarding.
- Even if a future operator enables mutation later, the daemon still requires a non-dry-run runtime
  and a successful heartbeat warmup cycle before moving a real VIP, and fails closed on heartbeat
  errors. Onboarding does nothing to weaken those controls.

## Failure Handling

A run can fail at any step. The design requires that failures be safe, legible, and recoverable.

### What a failed run leaves behind

Each step records what it changed so the operator knows the node's state:

- Failure in **Connect / Detect** leaves the target untouched.
- Failure in **Install prereqs / Fetch-build-install** may leave packages or a partial install on
  the target, but no config is written, the service is not enabled, and nothing moves a VIP.
- Failure in **Write config / Validate** leaves easy-failover installed but the service not enabled;
  an invalid config never reaches an enabled service because validation gates enablement.
- Failure in **Enable / Verify** leaves the service installed and, depending on the failure, possibly
  started — but always with `allow_network_mutation = false`, so a freshly onboarded node can never
  fight over a real VIP, validated or not.

The run summary states the reached step and the node's resulting state explicitly.

### Idempotency and retry

Steps are designed to be safe to re-run, so a failed run can be retried from the start or resumed:

- Prereq install is idempotent (already-present packages are skipped).
- Install detects an existing same-version binary and can be re-applied; this mirrors the installer's
  own "keep existing" behavior for config.
- Config write detects an existing `config.toml`. Like the source install flow,
  it must not clobber an operator-edited config silently: it either confirms the content matches what
  the dashboard would generate, or requires an explicit "overwrite" choice from the operator.
- Validate, enable, and verify are naturally repeatable.

Retry re-runs the sequence; completed idempotent steps converge quickly to `ok` and the run picks up
where it failed.

### Surfacing errors and manual recovery

- The failing step shows a clear, redacted error: the step label, the redacted command, the exit
  code, and captured stderr (run through the redaction rule).
- The operator can **retry** the run after fixing the cause (wrong password, missing repo access,
  unreachable peer), or fall back to **manual recovery** using the same documented manual flow in
  [`../README.md`](../README.md) (install, write config, `--validate-config`, enable the service).
- Because real VIP mutation stays disabled, a half-finished or abandoned onboarding never endangers
  an existing VIP owner. Cleanup of a partially installed node uses
  package removal (which keeps `config.toml` unless
  `--purge-config` is passed).

## Integration with the Authenticated Write API

Onboarding is a privileged remote write action and must ride on the authenticated write API designed
in the sibling document [`write-api-design.md`](write-api-design.md):

- The onboarding endpoint requires the same authentication and authorization as other dashboard
  write actions; it is not reachable from the read-only local API or from an unauthenticated session.
- It is disabled by default and explicitly enabled, matching the security notes in
  [`frontend-roadmap.md`](frontend-roadmap.md) and [`failover-safety.md`](failover-safety.md): no
  unauthenticated remote write, no privileged action exposed by default.
- Onboarding does not bypass the daemon's own safety controls. It provisions a node; it does not, and
  must not, expose a path to remote VIP mutation.

If [`write-api-design.md`](write-api-design.md) does not yet exist on this branch, treat this section
as the contract that document must satisfy; the link is by relative path to the sibling design.

## Deferred / Out of Scope

- Persisted SSH credentials, SSH agent forwarding, or unattended re-onboarding using a secret store.
- Bastion/jump-host chains, SSH certificate authorities, and host-key trust-on-first-use policy
  (host-key verification policy itself should be designed before implementation).
- Onboarding non-Linux targets (Windows/macOS are future platform targets in [`../README.md`](../README.md)).
- Distro-package installs (`apt`/`dnf`/`pacman` repositories) once packaging exists; today only
  source and release-tarball installs are covered.
- Fleet-wide rollout, batch onboarding, config drift reconciliation, and de-onboarding workflows.
- Enabling or tuning real VIP mutation, quorum, or fencing as part of onboarding.

## Status

This is a design document only. It defines the intended server-side SSH onboarding behavior and its
safety constraints. No onboarding code, daemon change, installer change, or API change is implied by
this file, and nothing here enables real VIP movement.
