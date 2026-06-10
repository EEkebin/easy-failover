# RPM %postun for the easy-failover DAEMON: remove local config on final erase (RPM has no
# separate purge). The dashboard package cleans up its own config/state/user.
# $1 == 0 on final erase; >= 1 during an upgrade.
if [ "$1" = "0" ]; then
    rm -rf /etc/easy-failover
    if command -v systemctl >/dev/null 2>&1; then
        systemctl daemon-reload || true
        systemctl reset-failed easy-failover.service || true
    fi
fi
exit 0
