# RPM %postun for the dashboard: drop config + user on final erase.
if [ "$1" = "0" ]; then
    rm -rf /etc/easy-failover-dashboard
    if getent passwd easy-failover-dashboard >/dev/null 2>&1; then
        userdel easy-failover-dashboard || true
    fi
    if command -v systemctl >/dev/null 2>&1; then
        systemctl daemon-reload || true
        systemctl reset-failed easy-failover-dashboard.service || true
    fi
fi
exit 0
