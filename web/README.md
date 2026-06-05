# easy-failover dashboard

This optional dashboard is a read-only Next.js app for local easy-failover state.

The daemon does not serve the local HTTP API yet, so the dashboard falls back to sample data that
matches the planned `/api/v1` response shapes.

## Run locally

```sh
npm install
npm run dev
```

Open <http://localhost:3000>.

## Build

```sh
npm run build
npm run lint
```

## API base

The dashboard reads from `http://127.0.0.1:8743` by default. Override it with:

```sh
NEXT_PUBLIC_EASY_FAILOVER_API_BASE=http://127.0.0.1:8743 npm run dev
```

The dashboard only reads these planned endpoints:

- `GET /api/v1/status`
- `GET /api/v1/config`
- `GET /api/v1/events`

It intentionally exposes no VIP mutation, daemon control, config write, or privileged action UI.
