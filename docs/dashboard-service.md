# Running the Dashboard as an Optional Service

easy-failover ships an optional Next.js dashboard **bundled in the same
`easy-failover` package** as the daemon. It runs as its own service, as a dedicated
unprivileged `easy-failover-dashboard` user, independent of the (root) daemon. This
document covers configuring the packaged dashboard and building/running it by hand.

The dashboard has **no login of its own**. It reads each node's local API and proxies
config writes using a server-side write token. Because anyone who can reach the dashboard
can therefore use its **Apply** button, the packaged dashboard binds **localhost only** by
default (see below).

## Comes with the package

Installing the `easy-failover` `.deb`/`.rpm` lays down, alongside the daemon, the
self-contained Next.js standalone server at `/usr/lib/easy-failover-dashboard`, its
unit `easy-failover-dashboard.service`, and seeds `/etc/easy-failover-dashboard/dashboard.env`
from the example. The package depends on `nodejs`. **The dashboard service is enabled
and started automatically on install**, and the daemon's local API is on by default, so
the dashboard works out of the box.

**Write access is auto-provisioned.** On first install the package:

- generates a random write token at `/etc/easy-failover/api.token` (root, `0600`);
- flips the daemon's seeded config to `read_only = false` with that `auth_token_file`;
- copies the token into the dashboard env as `EASY_FAILOVER_TOKEN_LOCAL`
  (`/etc/easy-failover-dashboard/dashboard.env`, `0640`, group `easy-failover-dashboard`);
- writes a roster entry for the local node at `/var/lib/easy-failover-dashboard/nodes.json`
  whose `tokenEnv` points at that variable.

So the dashboard's **Apply** works for the local node immediately — no manual token wiring.

**Exposure.** The daemon's read API binds `0.0.0.0:8743` (so remote read-only/fleet dashboards
can reach it; writes still require the token). The **dashboard** binds `127.0.0.1:3000` by
default, because it is unauthenticated and now write-capable — binding it to the network would
expose an unauthenticated write panel. Reach it remotely with an SSH tunnel:

```sh
ssh -L 3000:localhost:3000 user@node    # then open http://localhost:3000
```

Only set `HOSTNAME=0.0.0.0` in the dashboard env if you put real authentication or a reverse
proxy in front of it.

Configure it by editing the env file (listen address, which nodes to show, onboarding
gate) and restarting:

```sh
sudoedit /etc/easy-failover-dashboard/dashboard.env
sudo systemctl restart easy-failover-dashboard.service
```

`apt purge` / `dnf remove` stop and disable the service and remove its config directory
and the dedicated user. The unit is ordered `After=easy-failover.service` but does not
require it — the dashboard proxies to whatever nodes its roster lists. For a daemon-only
deployment (no dashboard, no `nodejs`), build the package with
`EASY_FAILOVER_NO_DASHBOARD=1 ./scripts/package.sh`.

## Build and run by hand

The rest of this document covers building the dashboard from source and running
it without the package (useful for development or custom deployments).

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
`0.0.0.0:8743` by default, reachable as `127.0.0.1:8743` from the same host), and
build with the matching base:

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

The package already installs and starts the dashboard as a service (see "Comes
with the package" above). For a hand-rolled deployment, the unit template and an
example env file live under [`packaging/dashboard/`](../packaging/dashboard/):

- [`easy-failover-dashboard.service.in`](../packaging/dashboard/easy-failover-dashboard.service.in)
  — a CMake `configure_file` template; the package substitutes the
  `@EASY_FAILOVER_DASHBOARD_*@` paths at build time. For a manual setup, replace
  those tokens with your install paths (e.g. `/usr/lib/easy-failover-dashboard`).
- [`easy-failover-dashboard.env.example`](../packaging/dashboard/easy-failover-dashboard.env.example)

The unit runs the prebuilt standalone server directly (`node server.js`), not
`npm run start`.

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
