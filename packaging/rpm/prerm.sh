# RPM %preun: stop the service and release the VIP on final erase only.
# $1 == 0 on final erase; >= 1 during an upgrade.
if [ "$1" = "0" ]; then
    if command -v systemctl >/dev/null 2>&1; then
        systemctl stop easy-failover.service || true
        systemctl disable easy-failover.service || true
    fi
    if [ -x /usr/bin/easy-failover ] && [ -e /etc/easy-failover/config.toml ]; then
        /usr/bin/easy-failover --config /etc/easy-failover/config.toml --release-vip || true
    fi
fi
exit 0
