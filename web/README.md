# easy-failover dashboard

This optional dashboard is a read-only Next.js app for local easy-failover state.

The dashboard reads the local HTTP API when the daemon is running with `api.enabled = true` and
falls back to sample data when the API is unavailable.

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

The dashboard only reads these endpoints:

- `GET /api/v1/status`
- `GET /api/v1/config`
- `GET /api/v1/events`

It intentionally exposes no VIP mutation, daemon control, config write, or privileged action UI.
