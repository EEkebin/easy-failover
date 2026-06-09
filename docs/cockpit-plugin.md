# Cockpit Plugin (`easy-failover-cockpit`)

An optional [Cockpit](https://cockpit-project.org/) module for managing an easy-failover
node from Cockpit's web console. It is shipped as a **separate package** that depends on
`easy-failover` and on Cockpit, so you install it only where you want it.

## Why a Cockpit plugin

The bundled standalone dashboard has no login of its own, so making it write-capable means
plumbing a bearer token and locking the dashboard to localhost. The Cockpit plugin sidesteps
that entirely: Cockpit provides the **system login**, **TLS**, and **privilege escalation**
(polkit / the superuser bridge). You log in to Cockpit as a system administrator, and the
plugin applies config and restarts the daemon **as you** — there is no separate API token to
manage.

Use the Cockpit plugin for single-host administration; the standalone dashboard
([`dashboard-service.md`](dashboard-service.md)) is still the better fit for a whole-fleet view.

## Install

```sh
# Debian/Ubuntu
sudo apt install ./easy-failover-cockpit_<version>_all.deb

# Fedora/RHEL
sudo dnf install ./easy-failover-cockpit-<version>-1.noarch.rpm
```

Both pull in `easy-failover` and `cockpit-bridge` if they are not already present. Make sure
Cockpit itself is running:

```sh
sudo systemctl enable --now cockpit.socket
```

Then open Cockpit (`https://<host>:9090`), log in as an administrator, and pick **Failover**
from the menu. The plugin's files live at `/usr/share/cockpit/easy-failover/`.

## What it does

- **Status** — reads the daemon's local API (`http://127.0.0.1:8743/api/v1/status`, unauthenticated
  on loopback) and shows the node's role, health, VIP, ownership, and lifecycle detail, refreshing
  every few seconds.
- **Set the virtual IP** — enter an address (CIDR) and interface and apply. The plugin:
  1. reads `/etc/easy-failover/config.toml` (via Cockpit's privileged file channel),
  2. sets `vip.address` / `vip.interface` in the `[vip]` table (leaving the rest of the file intact),
  3. validates the candidate via the daemon's `POST /api/v1/config/validate`,
  4. writes it back and restarts `easy-failover.service` (via the superuser bridge).

  Leaving both fields blank unconfigures the VIP (the daemon idles).

## Security model

- No bearer token is stored or used by the plugin. Authorization is Cockpit's: only a user who
  can log in to Cockpit **and** elevate (the superuser bridge prompts for it) can apply changes.
- The plugin talks to the daemon's read API over loopback only and never opens a network listener
  of its own (Cockpit serves it).
- It does not require the daemon's write API to be enabled; it edits the config file directly and
  restarts. (The auto-provisioned write token described in
  [`config-reference.md`](config-reference.md) is for the standalone dashboard, not this plugin.)

## Building from source

Cockpit plugins are static assets — no compile step. Build the package(s) with:

```sh
PKG_VERSION=<version> scripts/package-cockpit.sh   # -> build-cockpit/*.deb (+ *.rpm if rpmbuild present)
```

The source lives in [`cockpit/`](../cockpit/): `manifest.json`, `index.html`, `app.js`,
`easy-failover.css`.
