# RPM %postun for easy-failover-dashboard: remove its config/state and the user on final erase.
# $1 == 0 on final erase; >= 1 during an upgrade.
if [ "$1" = "0" ]; then
    rm -rf /etc/easy-failover-dashboard /var/lib/easy-failover-dashboard
    if getent passwd easy-failover-dashboard >/dev/null 2>&1; then
        userdel easy-failover-dashboard || true
    fi
    if command -v systemctl >/dev/null 2>&1; then
        systemctl daemon-reload || true
        systemctl reset-failed easy-failover-dashboard.service || true
    fi
fi
exit 0
