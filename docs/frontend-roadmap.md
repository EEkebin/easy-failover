# Frontend Roadmap

easy-failover includes an optional Next.js dashboard under `web/`.

The dashboard should talk to a future local API exposed by the agent. The API should be disabled
by default, bind to `127.0.0.1:8743`, and be read-only at first. The planned API contract is
tracked in [`local-api-design.md`](local-api-design.md).

Until the daemon has an HTTP listener, the dashboard renders sample data shaped like the planned
local API responses.

## Local Development

```sh
cd web
npm install
npm run dev
```

Build and lint:

```sh
cd web
npm run build
npm run lint
```

## Views

- Node status
- Current VIP owner
- Peer health
- Heartbeat state
- Recent failover events and logs
- Config view

## Possible API Endpoints

- `GET /api/v1/status`
- `GET /api/v1/config`
- `GET /api/v1/events`

## Security Notes

- Do not allow unauthenticated remote write access.
- Avoid exposing privileged actions over the network by default.
- Add token/auth support before write actions.
- Require explicit enablement for the dashboard/API.
