# Local API Design

easy-failover will eventually expose a local HTTP API for status inspection, configuration
visibility, validation, and recent runtime events. The current implementation evaluates API startup
configuration but does not open a listener or serve endpoints yet. This document defines the first
API shape before endpoint behavior is added.

## Scope

The first API implementation should be:

- disabled by default with `api.enabled = false`;
- bound to `127.0.0.1:8743` by default;
- read-only by default with `api.read_only = true`;
- versioned under `/api/v1`;
- JSON only;
- local process status only, with no cluster-wide truth claims beyond observed local runtime state.

The API must not expose ownership-changing actions, privileged VIP operations, shell command
execution, daemon control, config writes, or dashboard write actions in this first shape.

## Configuration

The existing `[api]` config section controls whether a future listener starts:

```toml
[api]
enabled = false
bind = "127.0.0.1:8743"
read_only = true
```

- `enabled`: when `false`, no socket is opened and the API startup state is disabled.
- `bind`: listener address used only when `enabled = true`.
- `read_only`: must stay `true` for this first API shape.

If `api.enabled = true` and `api.read_only = false`, the first API implementation should refuse to
start the API skeleton until write behavior and authentication are designed separately. This is an
API startup rule, not a general `Config::validate()` rule.

## Response Rules

All successful responses should use `Content-Type: application/json`.

Error responses should use this shape:

```json
{
  "error": {
    "code": "bad_request",
    "message": "request body must be valid JSON",
    "details": [
      "unexpected token at byte 1"
    ]
  }
}
```

Initial error codes should be stable strings, including:

- `bad_request`
- `not_found`
- `method_not_allowed`
- `api_disabled`
- `internal_error`

Responses should avoid exposing command output, environment variables, process arguments, or
secrets. Config fields must be treated as potentially sensitive because free-form strings can embed
credentials, tokens, URLs, headers, or shell arguments. The first API implementation should redact
or omit sensitive fields by default, including `health.command`.

## Endpoints

### `GET /api/v1/status`

Returns the local agent's current view of runtime state. The response model exists in code, but no
HTTP listener or route serves it yet.

Draft response:

```json
{
  "node": {
    "id": "node-a",
    "priority": 100,
    "state": "backup",
    "healthy": true
  },
  "vip": {
    "address": "10.0.0.50/24",
    "interface": "eth0",
    "local_owner": false
  },
  "lifecycle": {
    "initial_state": "stopped",
    "final_state": "stopped",
    "detail": "dry-run lifecycle iteration completed",
    "dry_run": true,
    "started": true,
    "iteration_ran": true,
    "stopped": true
  },
  "heartbeat": {
    "bind": "0.0.0.0:7432",
    "interval_ms": 1000,
    "timeout_ms": 3000,
    "peers_observed": 0
  },
  "health": {
    "status": "healthy",
    "detail": "no health command configured"
  }
}
```

`local_owner` should remain `false` until real VIP ownership state exists. It must not be inferred
from election helpers alone.

When a health command is configured, `health.detail` should use a redaction marker instead of
returning command output or runner error text.

### `GET /api/v1/config`

Returns the effective runtime configuration after defaults are applied. The response model exists in
code, but no HTTP listener or route serves it yet.

Draft response:

```json
{
  "node_id": "node-a",
  "priority": 100,
  "vip": {
    "address": "10.0.0.50/24",
    "interface": "eth0"
  },
  "heartbeat": {
    "bind": "0.0.0.0:7432",
    "interval_ms": 1000,
    "timeout_ms": 3000
  },
  "health": {
    "command_redacted": true,
    "interval_ms": 1000,
    "timeout_ms": 2000
  },
  "election": {
    "require_quorum": false,
    "preempt": true
  },
  "api": {
    "enabled": false,
    "bind": "127.0.0.1:8743",
    "read_only": true
  },
  "peers": [
    {
      "id": "node-b",
      "address": "10.0.0.12:7432"
    }
  ]
}
```

The endpoint should return the effective in-memory config, not raw TOML. Free-form command strings
and future sensitive values should be omitted or replaced with explicit redaction markers rather
than returned verbatim.

### `POST /api/v1/config/validate`

Validates a submitted candidate TOML config without applying it. This is the only planned `POST`
in the first API shape, and it is still read-only because it does not mutate runtime state. The
request/response model exists in code, but no HTTP listener or route serves it yet.

Draft request:

```json
{
  "format": "toml",
  "config": "[vip]\naddress = \"10.0.0.50/24\"\ninterface = \"eth0\"\n"
}
```

Draft response:

```json
{
  "valid": false,
  "errors": [
    "at least one peer is required"
  ]
}
```

The validator should use the same config loading and validation rules as `--validate-config`.

Validation should return HTTP `200` with `valid=false` when the submitted TOML parses and validation
finds config errors. The standard error envelope is for request/API errors, such as malformed JSON,
unsupported `format`, missing `config`, oversized request body, or internal failures. Initial status
code guidance:

- `200`: request was well-formed and validation completed, regardless of whether the candidate
  config is valid;
- `400`: malformed JSON, missing required fields, unsupported `format`, or TOML parse failure;
- `413`: request body exceeds the configured validation limit;
- `500`: unexpected validation service failure.

### `GET /api/v1/events`

Returns recent structured runtime events from an in-memory ring buffer once such a buffer exists.
Until then, the endpoint can return an empty list.

Draft response:

```json
{
  "events": [
    {
      "sequence": 1,
      "event": "daemon_lifecycle_result",
      "level": "info",
      "message": "event=daemon_lifecycle_result node_id=\"node-a\" initial_state=stopped final_state=stopped started=true iteration_ran=true stopped=true dry_run=true validation_errors=0 vip_operations=2 detail=\"dry-run lifecycle iteration completed\"",
      "fields": {
        "node_id": "node-a",
        "initial_state": "stopped",
        "final_state": "stopped",
        "started": true,
        "iteration_ran": true,
        "stopped": true,
        "dry_run": true,
        "validation_errors": 0,
        "vip_operations": 2,
        "detail": "dry-run lifecycle iteration completed"
      }
    }
  ]
}
```

Events should correspond to structured runtime log fields when possible. The endpoint should bound
its memory usage and support a future `?limit=` parameter.

## Deferred Work

The following are intentionally out of scope for the first API shape:

- write endpoints;
- remote VIP ownership changes;
- daemon start/stop/reload controls;
- config file writes or hot reload;
- authentication and authorization implementation;
- TLS;
- dashboard implementation;
- exposing privileged command output;
- cluster-wide consensus state.

Authentication must be designed before any remote write action is added. A future dashboard should
consume this API but should not expand the API's privilege boundary by itself.
