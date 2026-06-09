# RPM %post: seed config, refresh systemd, set up the bundled dashboard.
# $1 == 1 on first install, 2+ on upgrade.
if [ ! -e /etc/easy-failover/config.toml ] \
    && [ -e /etc/easy-failover/config.example.toml ]; then
    cp /etc/easy-failover/config.example.toml /etc/easy-failover/config.toml
    chmod 0644 /etc/easy-failover/config.toml
fi

# Bundled dashboard (present only when built with it): create user, seed env.
if [ -e /usr/lib/easy-failover-dashboard/server.js ]; then
    if ! getent passwd easy-failover-dashboard >/dev/null 2>&1; then
        useradd --system --no-create-home --shell /sbin/nologin \
            --comment "easy-failover dashboard" easy-failover-dashboard || true
    fi
    if [ ! -e /etc/easy-failover-dashboard/dashboard.env ] \
        && [ -e /etc/easy-failover-dashboard/dashboard.env.example ]; then
        cp /etc/easy-failover-dashboard/dashboard.env.example \
            /etc/easy-failover-dashboard/dashboard.env
        chmod 0644 /etc/easy-failover-dashboard/dashboard.env
    fi
fi

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || true
    # Auto-start the daemon; it idles until a VIP is configured (clean-slate config). Clear any
    # failed/start-limited state from a previous version, then enable (for boot) and start
    # separately so a tripped `enable` still leaves the unit running.
    systemctl reset-failed easy-failover.service 2>/dev/null || true
    systemctl enable easy-failover.service 2>/dev/null || true
    systemctl start easy-failover.service || true
    if [ -e /usr/lib/easy-failover-dashboard/server.js ]; then
        systemctl reset-failed easy-failover-dashboard.service 2>/dev/null || true
        systemctl enable easy-failover-dashboard.service 2>/dev/null || true
        systemctl start easy-failover-dashboard.service || true
    fi
fi
exit 0
