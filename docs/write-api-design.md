# Authenticated Write API Design

This document designs the authentication and authorization model for adding *write* and *control*
operations to the easy-failover local API. It builds on the read-only contract in
[`local-api-design.md`](local-api-design.md) and the safety posture in
[`failover-safety.md`](failover-safety.md). Config field semantics live in
[`config-reference.md`](config-reference.md).

This is a **design document only**. It does not add runtime code, change existing endpoints, or
enable writes. The current API remains read-only and disabled by default until the work described
here is implemented and reviewed.

## Goals and Non-Goals

The first authenticated write API should:

- keep the existing safe-by-default posture: read-only and disabled unless explicitly opted in;
- make it *impossible* to enable mutations without configuring authentication;
- authenticate every write/control request with a token before any state changes;
- reject unauthenticated mutations with the existing JSON error envelope;
- audit every write/control attempt, success or failure, as a structured runtime event;
- bind to loopback only, exactly like the read-only API, so the first version is not remotely
  reachable;
- give the Next.js dashboard a way to perform writes without ever exposing the token to the browser.

It is explicitly **not** a goal to add TLS, mTLS, multi-user accounts, RBAC, or a remote network bind
in this first shape. Those are deferred (see [Deferred / Out of Scope](#deferred--out-of-scope)).

## Threat Model and Posture

The read-only API already treats config strings as potentially sensitive and redacts privileged
command output. A write API raises the stakes: write and control endpoints can change effective
config and, in the future, daemon lifecycle. The guardrails in
[`failover-safety.md`](failover-safety.md) require that any API able to affect ownership must be
explicitly enabled, authenticated, and never expose unauthenticated remote writes.

The first version assumes a **single trusted operator** on the local host (or a confined dashboard
backend on the same host). The token is a shared secret between that operator and the daemon. The
trust boundary is the loopback interface plus filesystem permissions on the token file. Anyone who
can read the token file or send loopback requests with the token is treated as authorized. Broader
multi-user and remote scenarios are deferred.

## Token-Based Authentication

The first implementation uses a single bearer token.

### Provisioning

The token is provided through a **token file** referenced by a new `[api]` field,
`auth_token_file`:

```toml
[api]
enabled = true
bind = "127.0.0.1:8743"
read_only = false
auth_token_file = "/etc/easy-failover/api-token"
```

- The file contains a single secret token (trimmed of trailing newline/whitespace). It is generated
  by the operator, for example with `head -c 32 /dev/urandom | base64`.
- The file must be readable only by the daemon user, for example mode `0600` (or `0640` with a
  dedicated group). At startup the daemon should warn or refuse if the token file is group- or
  world-readable, consistent with the conservative posture.
- A configured-secret alternative (an inline `auth_token` value in TOML) is intentionally *not*
  offered first: it would risk the secret leaking into config dumps, the `GET /api/v1/config`
  response, logs, and backups. The file indirection keeps the secret out of the config surface, and
  the existing redaction rules in [`local-api-design.md`](local-api-design.md) continue to apply.
- The token is loaded once at startup into daemon memory. The loaded value must never be serialized
  into any API response and must be redacted/omitted from `GET /api/v1/config` (the response should
  expose only whether a token file is configured, never its contents).

### Presentation

Clients present the token in the standard HTTP `Authorization` header using the `Bearer` scheme:

```http
POST /api/v1/config/apply HTTP/1.1
Host: 127.0.0.1:8743
Authorization: Bearer s3cr3t-token-value
Content-Type: application/json
```

### Verification

- The daemon compares the presented token against the loaded token using a **constant-time**
  comparison (compare over a fixed length so timing does not reveal how many leading bytes matched).
  A naive `==` / `std::string` comparison must not be used on the secret path.
- Comparison is exact and case-sensitive; no normalization beyond trimming the configured token file
  value at load time.
- The token value is never logged, never echoed back in error messages, and never included in audit
  events (see [Audit Logging](#audit-logging)).

### Failure Responses

Authentication failures reuse the existing error envelope from
[`local-api-design.md`](local-api-design.md) with new stable codes:

- **Missing token** on a write/control endpoint -> HTTP `401` with code `unauthorized`.
- **Malformed `Authorization` header** (not a `Bearer` scheme, empty token) -> HTTP `401` with code
  `unauthorized`.
- **Present but incorrect token** -> HTTP `403` with code `forbidden`.

```json
{
  "error": {
    "code": "unauthorized",
    "message": "authentication required for write operations"
  }
}
```

```json
{
  "error": {
    "code": "forbidden",
    "message": "invalid credentials"
  }
}
```

The `details` array is omitted on auth failures so the response never leaks why a token was rejected
beyond the missing/invalid distinction. Both new codes are added to the API's stable error code set
alongside `bad_request`, `not_found`, `method_not_allowed`, `api_disabled`, and `internal_error`.

## Read/Write Mode Gating

Today the startup logic in `evaluateLocalApiStartup` rejects startup when `api.enabled = true` and
`api.read_only = false`, because write behavior and authentication were undesigned. This design
replaces that unconditional rejection with a gated opt-in.

Write mode requires **both** conditions, and enabling writes without authentication must be
impossible:

1. `api.read_only = false`, and
2. `api.auth_token_file` set to a readable token file containing a non-empty secret.

Proposed startup rules:

- `enabled = false` -> API disabled (unchanged).
- `enabled = true`, `read_only = true` -> read-only listener (unchanged; `auth_token_file` may be
  absent because GETs stay unauthenticated in read-only mode).
- `enabled = true`, `read_only = false`, `auth_token_file` missing/empty/unreadable -> **startup
  rejected**. The daemon must fail closed with a clear detail such as
  `"write mode requires a readable api.auth_token_file"`. This guarantees an API with no token
  configured can never accept mutations.
- `enabled = true`, `read_only = false`, valid `auth_token_file` -> write-enabled listener; GETs
  remain available and all write/control paths require a valid bearer token.

`read_only = true` stays the documented default, so the safe-by-default behavior is preserved. The
write path is strictly opt-in and additive. This stays an API startup rule (a new state on the
`LocalApiStartupState` model) rather than a general `Config::validate()` rule, matching how the
existing `read_only` gate is described.

## Authorization of Requests

In write-enabled mode:

- **Read endpoints** (`GET /api/v1/status`, `GET /api/v1/config`, `GET /api/v1/events`) **stay
  unauthenticated**. They already redact sensitive values and expose only local observed state, and
  the listener is loopback-only. Keeping them open avoids forcing the token onto every dashboard
  read and matches the current read-only contract. (A future hardening option is to require auth for
  reads too; that is deferred.)
- **Write and control endpoints** require a valid bearer token. Any such request without a valid
  token is rejected with `401`/`403` *before* the handler runs and before any mutation is attempted.
  There is no "soft" path: an unauthenticated mutation always returns an auth error and changes
  nothing.

This split is the explicit design choice: GETs open, mutations authenticated.

## First Write Endpoints Unlocked

This auth/authz model is the prerequisite for, but does not specify the bodies of, the first write
endpoints. At a high level it unlocks:

- **Config write/apply** (the subject of issue #97): authenticated endpoints to write a validated
  config and apply/reload it into the running daemon. The existing `POST /api/v1/config/validate`
  remains read-only and unauthenticated; the new apply path is the first true mutation and is
  authenticated. Apply must still run the same validation as `--validate-config` and respect the
  safety controls in [`failover-safety.md`](failover-safety.md).
- **Daemon control** (reload/restart and similar lifecycle actions): noted as a **future** authorized
  capability built on the same token model, not part of the first write shape.

Request/response schemas for these endpoints are out of scope here and belong to #97 and follow-up
control-API issues.

## Audit Logging

Every write/control attempt emits a structured **logfmt** runtime event, matching the existing
`daemon_lifecycle_result` and `vip_operation` style in `src/runtime/RuntimeLog.cpp` (space-separated
`key=value`, quoted string values). The event is emitted for **both success and failure**, including
auth rejections, so attempts are auditable even when nothing changes.

Proposed event shape (`api_write_attempt`):

```text
event=api_write_attempt actor="bearer-token" outcome=rejected reason=unauthorized target="config_apply" method=POST path="/api/v1/config/apply" status=401 dry_run=false detail="missing bearer token"
```

Field guidance:

- `actor`: the authenticated principal class. For the first token model this is a fixed label such
  as `"bearer-token"`. It must **never** contain the token value or any secret. When RBAC/multi-user
  arrives, this field can carry a key id or user id (still never the secret).
- `outcome`: `applied` / `accepted` / `rejected` / `error`.
- `reason`: a stable reason such as `ok`, `unauthorized`, `forbidden`, `validation_failed`,
  `internal_error`.
- `target`: the logical operation, for example `config_apply`, `daemon_reload`.
- `method`, `path`, `status`: HTTP method, request path, and response status code.
- `detail`: short human-readable summary; redacted like other runtime events, never echoing config
  secrets, command output, or the token.

These events should also be representable in the `GET /api/v1/events` feed via the existing
`LocalApiEvent` field model, so the dashboard can show write/control history alongside lifecycle and
VIP events.

## Dashboard Usage

The Next.js dashboard under `web/` must hold and send the token **server-side only**, never exposing
it to browser code. This matches the project's SSH-onboarding posture: credentials live in the
server runtime and never cross into the browser bundle, client state, or network responses sent to
the client.

Proposed flow:

- The token is read by the dashboard's server runtime from a server-only secret (for example an
  environment variable or a server-readable file), never from `NEXT_PUBLIC_*` env vars and never
  shipped in client-side JavaScript.
- The browser calls the dashboard's own server-side route handlers / server actions. Those run on
  the Node side and attach `Authorization: Bearer <token>` when proxying the request to the
  loopback easy-failover API.
- The token is never placed in client-fetched responses, cookies readable by JS, HTML, or logs that
  reach the browser. Browser code only ever sees the *result* of a write, not the credential.
- Because the easy-failover API binds to loopback, the dashboard backend must run on the same host
  (or reach the daemon through an operator-managed secure channel). Direct browser-to-daemon writes
  are not supported.

## Future TLS / mTLS Considerations

The first version **binds loopback only** (`127.0.0.1:8743`) and relies on OS process/loopback
isolation plus the bearer token. On loopback there is no untrusted network segment to protect, so
plaintext HTTP with a bearer token is acceptable for the first shape, and adding TLS would mainly add
operational cost (certificates, rotation) without a matching threat reduction.

This changes as soon as remote or cross-node access is considered:

- A bearer token sent over a non-loopback link **must** be protected by TLS, or it is exposed to
  network sniffing. So any future remote bind requires TLS as a hard prerequisite.
- **mTLS** is the preferred later option for cross-node/control-plane access: each node presents a
  client certificate, giving mutual authentication and a natural path to per-node identity in audit
  events (`actor` becomes a certificate subject). This is a stronger model than a single shared
  bearer token for multi-node deployments.
- Until TLS/mTLS exists, the API must not be bound to a non-loopback address for write mode. The
  startup logic should keep the conservative default and treat remote bind + writes as a future,
  separately-reviewed capability.

## Deferred / Out of Scope

The following are intentionally **not** part of this first authenticated write shape:

- **TLS** for the local listener (loopback-only, plaintext + bearer token first).
- **mTLS** / client certificates and per-node certificate identity.
- **Multi-user accounts and RBAC** (the first model is a single shared token with a fixed `actor`).
- **Remote / non-loopback bind** for write mode, and any cross-node control plane.
- **Token rotation automation**, multiple concurrent tokens, and revocation lists (rotation is a
  manual file replacement plus restart in the first shape).
- **Endpoint request/response bodies** for config apply (#97) and daemon control, which are specified
  in their own issues.

This document is **design only**. No code, config defaults, or endpoints change until the model above
is implemented and reviewed.
