# Local API Design

easy-failover will eventually expose a local HTTP API for status inspection, configuration
visibility, validation, and recent runtime events. This document defines the first API shape before
any listener or server implementation is added.

## Scope

The first API implementation should be:

- disabled by default with `api.enabled = false`;
- bound to `127.0.0.1:8743` by default;
- read-only by default with `api.read_only = true`;
- versioned under `/api/v1`;
- JSON only;
- local-process status only, with no cluster-wide truth claims beyond observed local runtime state.

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

- `enabled`: when `false`, no socket is opened.
- `bind`: listener address used only when `enabled = true`.
- `read_only`: must stay `true` for this first API shape.

If `api.enabled = true` and `api.read_only = false`, the first API implementation should reject the
configuration until write behavior and authentication are designed separately.

## Response Rules

All successful responses should use `Content-Type: application/json`.

Error responses should use this shape:

```json
{
  "error": {
    "code": "validation_failed",
    "message": "config validation failed",
    "details": []
  }
}
```

Initial error codes should be stable strings, including:

- `not_found`
- `method_not_allowed`
- `validation_failed`
- `api_disabled`
- `internal_error`

Responses should avoid exposing command output, environment variables, process arguments, or
secrets. The current config model does not contain secrets, but future fields should be classified
before they are returned.

## Endpoints

### `GET /api/v1/status`

Returns the local agent's current view of runtime state.

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
    "state": "stopped",
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
    "detail": "health command is not configured"
  }
}
```

`local_owner` should remain `false` until real VIP ownership state exists. It must not be inferred
from election helpers alone.

### `GET /api/v1/config`

Returns the effective runtime configuration after defaults are applied.

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
    "command": "curl -fsS http://127.0.0.1:8080/health",
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

The endpoint should return the effective in-memory config, not raw TOML.

### `POST /api/v1/config/validate`

Validates a submitted candidate TOML config without applying it. This is the only planned `POST`
in the first API shape, and it is still read-only because it does not mutate runtime state.

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
      "message": "event=daemon_lifecycle_result node_id=\"node-a\" final_state=stopped",
      "fields": {
        "node_id": "node-a",
        "final_state": "stopped"
      }
    }
  ]
}
```

Events should correspond to structured runtime log fields when possible. The endpoint should bound
memory use and support a future `?limit=` parameter.

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
