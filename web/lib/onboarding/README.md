# SSH Onboarding Backend

Server-side SSH onboarding for easy-failover nodes, implementing
[`docs/ssh-onboarding-design.md`](../../../docs/ssh-onboarding-design.md). This is
the **backend only** (issue #100); the dashboard wizard UI is a separate issue
(#101). It is callable from the server route at
[`web/app/api/onboard/route.ts`](../../app/api/onboard/route.ts).

> **Server-only.** Every module here runs on the dashboard server. `ssh.ts`
> imports `ssh2`, a Node-only dependency, and must never be imported into a
> client component. The route handler pins itself to the Node.js runtime
> (`export const runtime = "nodejs"`). Secrets never reach the browser and are
> never baked into the client build.

## Modules

| File | Purpose | Pure? |
| --- | --- | --- |
| `types.ts` | `OnboardRequest`, progress/result, and node-state types. | n/a |
| `config.ts` | Renders a deterministic `config.toml` from request fields. | yes |
| `redact.ts` | Builds a redactor that scrubs secrets to `***`. | yes |
| `plan.ts` | Pure per-step command builders + detection parsing. | yes |
| `ssh.ts` | Thin ssh2 layer behind a `RemoteRunner` interface; host-key policy. | no (ssh2) |
| `onboard.ts` | Orchestrates the step sequence; emits redacted progress. | logic testable via fake runner |

## Step sequence

`connect → detect → install-prereqs → fetch-build-install → write-config →
validate-config → enable-start → verify`. Each step yields a redacted progress
event and the sequence stops on the first failure, recording the node's
resulting state. The generated config always forces
`[mutation_safety].allow_network_mutation = false`, so an onboarded node never
moves a real VIP.

## Host-key policy: TOFU-with-pinning + optional strict mode

- **Strict** — if the caller supplies `connection.hostKey.expectedFingerprint`
  (a `SHA256:...` base64 fingerprint), the server's presented host key is
  verified against it during the SSH handshake. A mismatch **aborts** the run at
  the Connect step (`HostKeyMismatchError`); the target is left untouched.
- **TOFU** — if no fingerprint is supplied, the key is accepted on first connect
  but its fingerprint is **pinned** and returned in the result
  (`summary.hostKeyFingerprint`) so the caller can store it and pass it as
  `expectedFingerprint` on later connections.
- A changed key is **never** silently accepted when a fingerprint was supplied.

## Redaction

`createRedactor([...secrets])` closes over the run's ephemeral secrets (SSH
password, private key, passphrase, sudo password) and replaces every occurrence
of each with `***`. Every progress/log/detail/command/stderr string passes
through it before leaving the server. Secrets travel **out-of-band**: SSH auth
via the ssh2 connect config, and the sudo password via `sudo -S` on stdin —
never as argv. Redaction is the defense-in-depth backstop.

## Ephemeral credentials

Credentials are held only for the duration of one `onboard()` call. They are
never written to disk, a database, or any cache, and never returned to the
client. The SSH connection (and the secrets it holds) is closed in a `finally`
block as soon as the run finishes or fails.

## Authorization

The route is a privileged remote write action. It is **disabled by default**
(`EASY_FAILOVER_ONBOARD_ENABLED` must be `true`) and `authorizeWriteRequest` is
the single extension point that MUST be wired to the authenticated write API
(see [`docs/write-api-design.md`](../../../docs/write-api-design.md)) before the
route is exposed in any deployment.

## Testing / E2E note

This backend cannot be end-to-end tested here: there is no target host, and Next
has no test runner in this repo. Verification is build/lint/typecheck plus the
purity of `config.ts`, `redact.ts`, and `plan.ts` (designed to be unit-testable
with a fake `RemoteRunner`). **Real-host onboarding (an actual SSH install on a
reachable Linux target) is untested** and requires a live target host.
