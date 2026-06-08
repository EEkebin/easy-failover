# RPM %preun for the dashboard: stop+disable on final erase only.
if [ "$1" = "0" ]; then
    if command -v systemctl >/dev/null 2>&1; then
        systemctl stop easy-failover-dashboard.service || true
        systemctl disable easy-failover-dashboard.service || true
    fi
fi
exit 0
