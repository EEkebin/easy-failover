# RPM %post for the easy-failover DAEMON: seed config, provision a write token, start the daemon.
# $1 == 1 on first install, 2+ on upgrade.
TOKEN_FILE=/etc/easy-failover/api.token

seeded_config=0
if [ ! -e /etc/easy-failover/config.toml ] \
    && [ -e /etc/easy-failover/config.example.toml ]; then
    cp /etc/easy-failover/config.example.toml /etc/easy-failover/config.toml
    chmod 0644 /etc/easy-failover/config.toml
    seeded_config=1
fi

# Generate a random write token once (root-only); the optional dashboard package reads the same
# file to wire its Apply button.
if [ ! -e "$TOKEN_FILE" ]; then
    ( umask 077
      if command -v openssl >/dev/null 2>&1; then
          openssl rand -hex 32 > "$TOKEN_FILE"
      else
          head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$TOKEN_FILE"
      fi )
    chmod 0600 "$TOKEN_FILE"
fi
TOKEN="$(cat "$TOKEN_FILE" 2>/dev/null || true)"

# On a freshly seeded config, enable token-authenticated write mode. Never rewrite an operator's
# existing config.
if [ "$seeded_config" = "1" ] && [ -n "$TOKEN" ]; then
    sed -i \
        -e 's|^[[:space:]]*read_only[[:space:]]*=.*|read_only = false|' \
        -e "s|^[[:space:]]*auth_token_file[[:space:]]*=.*|auth_token_file = \"${TOKEN_FILE}\"|" \
        /etc/easy-failover/config.toml || true
fi

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || true
    # Auto-start the daemon; it idles until a VIP is configured (clean-slate config). Clear any
    # failed/start-limited state, then enable (for boot) and start separately.
    systemctl reset-failed easy-failover.service 2>/dev/null || true
    systemctl enable easy-failover.service 2>/dev/null || true
    systemctl start easy-failover.service || true
fi
exit 0
