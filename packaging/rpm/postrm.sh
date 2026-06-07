# RPM %postun: remove local config on final erase (RPM has no separate purge).
# $1 == 0 on final erase; >= 1 during an upgrade.
if [ "$1" = "0" ]; then
    rm -rf /etc/easy-failover
    if command -v systemctl >/dev/null 2>&1; then
        systemctl daemon-reload || true
    fi
fi
exit 0
