# easy-failover dashboard packaging

These files build the dashboard into the main `easy-failover` package (the
`dashboard` CPack component) and back its systemd service. The package bundles a
prebuilt Next.js standalone server and runs it as the unprivileged
`easy-failover-dashboard` user.

Files:

- `easy-failover-dashboard.service.in` — CMake `configure_file` template for the
  systemd unit. The package substitutes the `@EASY_FAILOVER_DASHBOARD_*@` paths
  at build time; ExecStart runs the standalone server (`node server.js`), not
  `npm run start`.
- `easy-failover-dashboard.env.example` — installed as
  `/etc/easy-failover-dashboard/dashboard.env.example` and seeded to
  `dashboard.env` on first install.

Build a daemon-only package (no dashboard, no `nodejs` dependency) with
`EASY_FAILOVER_NO_DASHBOARD=1 ./scripts/package.sh`.

See [`docs/dashboard-service.md`](../../docs/dashboard-service.md) for the full
walkthrough.
