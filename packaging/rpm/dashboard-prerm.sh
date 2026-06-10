# RPM %preun for easy-failover-dashboard: stop the service on final erase only.
# $1 == 0 on final erase; >= 1 during an upgrade.
if [ "$1" = "0" ]; then
    if command -v systemctl >/dev/null 2>&1; then
        systemctl stop easy-failover-dashboard.service || true
        systemctl disable easy-failover-dashboard.service || true
    fi
fi
exit 0
