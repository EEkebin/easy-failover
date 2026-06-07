# RPM %post: seed an active config from the example and refresh systemd.
# $1 == 1 on first install, 2+ on upgrade.
if [ ! -e /etc/easy-failover/config.toml ] \
    && [ -e /etc/easy-failover/config.example.toml ]; then
    cp /etc/easy-failover/config.example.toml /etc/easy-failover/config.toml
    chmod 0644 /etc/easy-failover/config.toml
fi
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || true
fi
# Not enabled/started automatically: edit /etc/easy-failover/config.toml and
# validate it, then `systemctl enable --now easy-failover.service`.
exit 0
