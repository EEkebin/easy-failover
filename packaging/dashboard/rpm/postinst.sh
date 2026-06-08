# RPM %post for the dashboard: create the service user, seed env.
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
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || true
fi
exit 0
