# Running the Dashboard as an Optional Service

easy-failover includes an optional read-only Next.js dashboard under `web/`. It
is separate from the daemon binary and is **not** built or installed by CMake.
This document describes how to build it and run it as a long-running service on a
node, pointed at that node's local API.

The dashboard exposes no privileged actions: it does not perform VIP mutation,
daemon control, config writes, or any other write action. It only reads the
local API (`GET /api/v1/status`, `GET /api/v1/config`, `GET /api/v1/events`) and
falls back to built-in sample data when that API is unavailable. Running it is
entirely optional.

## Prerequisites

- **Node.js 20 or newer.** The dashboard targets Next.js 16 and React 19, which
  require Node 20+.
- A built copy of the dashboard (the contents of `web/`).

## Build

From the `web/` directory:

```sh
cd web
npm ci        # or: npm install
npm run build
```

`npm ci` installs the exact dependencies from the lockfile and is preferred for
reproducible/service deployments; `npm install` works when no lockfile is
present or you intend to update dependencies.

### Configure the API base (set before building)

The dashboard reads its API base from the `NEXT_PUBLIC_EASY_FAILOVER_API_BASE`
environment variable. The default is `http://127.0.0.1:8743`.

`NEXT_PUBLIC_*` variables in Next.js are **public, build-time** values: they are
baked into the compiled output during `npm run build`. You must therefore set
`NEXT_PUBLIC_EASY_FAILOVER_API_BASE` *before* building, not just at runtime.
Changing it later requires rebuilding the dashboard.

To serve the dashboard on a node and point it at that node's local API, run the
daemon on that node with `api.enabled = true` (the API binds to
`127.0.0.1:8743` by default), and build with the matching base:

```sh
cd web
NEXT_PUBLIC_EASY_FAILOVER_API_BASE=http://127.0.0.1:8743 npm run build
```

If the API is not reachable (for example the daemon is not running with
`api.enabled = true`), the dashboard renders sample data shaped like the local
API responses, so it still loads.

## Run the production server

After building, start Next.js's production server:

```sh
cd web
npm run start
```

This serves the dashboard on port `3000` by default (override with the `PORT`
environment variable). The `start` script runs `next start`.

## Run as a systemd service

A unit template and an example environment file are provided under
[`packaging/dashboard/`](../packaging/dashboard/):

- [`easy-failover-dashboard.service`](../packaging/dashboard/easy-failover-dashboard.service)
- [`easy-failover-dashboard.env.example`](../packaging/dashboard/easy-failover-dashboard.env.example)
- [`packaging/dashboard/README.md`](../packaging/dashboard/README.md)

Because the dashboard is not CMake-installed, the unit uses documented
placeholder paths rather than `@`-substituted CMake variables. Adjust them for
your deployment.

### Install layout

1. Copy or check out `web/` to a stable location, for example
   `/opt/easy-failover-dashboard`, and build it there (see above). The built
   `.next/` output, `node_modules/`, and `package.json` must remain in that
   directory.
2. Create a dedicated, unprivileged user to run the service:

   ```sh
   sudo useradd --system --no-create-home --shell /usr/sbin/nologin \
       easy-failover-dashboard
   ```

3. Install the environment file (optional):

   ```sh
   sudo install -D -m 0644 \
       packaging/dashboard/easy-failover-dashboard.env.example \
       /etc/easy-failover/dashboard.env
   ```

   Remember that `NEXT_PUBLIC_EASY_FAILOVER_API_BASE` in the env file only
   matters if it matches what was set at build time; rebuild after changing it.

4. Install the unit, editing `WorkingDirectory` (and `User`/`Group` and the
   `npm` path if needed) to match your install:

   ```sh
   sudo install -D -m 0644 \
       packaging/dashboard/easy-failover-dashboard.service \
       /etc/systemd/system/easy-failover-dashboard.service
   sudo systemctl daemon-reload
   sudo systemctl enable --now easy-failover-dashboard.service
   ```

### Hardening

The unit runs as a dedicated unprivileged user with `NODE_ENV=production`,
`Restart=on-failure`, and `After=network-online.target`. Since the dashboard is
read-only and needs no special privileges, the unit stays minimal but applies
reasonable hardening: `NoNewPrivileges`, `ProtectSystem=strict`, `ProtectHome`,
and `PrivateTmp`. The daemon's own unit
([`packaging/systemd/easy-failover.service.in`](../packaging/systemd/easy-failover.service.in))
applies heavier hardening because it manages host networking; the dashboard does
not need those network capabilities.

## See also

- [`README.md` — Optional Dashboard](../README.md#optional-dashboard)
- [`web/README.md`](../web/README.md)
- [Frontend Roadmap](frontend-roadmap.md)
- [Local API Design](local-api-design.md)
