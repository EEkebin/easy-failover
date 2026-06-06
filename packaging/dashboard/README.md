# easy-failover dashboard packaging

This directory provides an optional runtime path for the read-only Next.js
dashboard under `web/`. The dashboard is separate from the daemon binary and is
**not** built or installed by CMake; these files document and template a manual,
opt-in deployment.

Files:

- `easy-failover-dashboard.service` — systemd unit template that runs the built
  dashboard with `npm run start`. Uses documented placeholder paths instead of
  CMake substitution.
- `easy-failover-dashboard.env.example` — example environment file.

See [`docs/dashboard-service.md`](../../docs/dashboard-service.md) for the full
install, build, and configuration walkthrough.
