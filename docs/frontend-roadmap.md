# Frontend Roadmap

easyFailover may eventually include an optional Next.js dashboard under `web/`.
No frontend implementation exists yet.

The dashboard should talk to a future local API exposed by the agent. The API should be disabled
by default, bind to `127.0.0.1` by default, and be read-only at first.

## Planned Views

- Node status
- Current VIP owner
- Peer health
- Heartbeat state
- Recent failover events and logs
- Config view
- Config validation
- Safe config editing and application later

## Possible API Endpoints

- `GET /api/v1/status`
- `GET /api/v1/config`
- `POST /api/v1/config/validate`
- `GET /api/v1/events`

## Security Notes

- Do not allow unauthenticated remote write access.
- Avoid exposing privileged actions over the network by default.
- Add token/auth support before write actions.
- Require explicit enablement for the dashboard/API.
