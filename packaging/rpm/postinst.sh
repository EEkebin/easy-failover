# RPM %post: seed config, provision a write token, set up the bundled dashboard.
# $1 == 1 on first install, 2+ on upgrade.
TOKEN_FILE=/etc/easy-failover/api.token
DASH_USER=easy-failover-dashboard
DASH_ENV=/etc/easy-failover-dashboard/dashboard.env
DASH_STATE=/var/lib/easy-failover-dashboard
DASH_ROSTER="${DASH_STATE}/nodes.json"

seeded_config=0
if [ ! -e /etc/easy-failover/config.toml ] \
    && [ -e /etc/easy-failover/config.example.toml ]; then
    cp /etc/easy-failover/config.example.toml /etc/easy-failover/config.toml
    chmod 0644 /etc/easy-failover/config.toml
    seeded_config=1
fi

# Generate a random write token once (root-only); the local dashboard is handed the same
# value so its Apply button works out of the box.
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

# On a freshly seeded config, enable token-authenticated write mode. Never rewrite an
# operator's existing config.
if [ "$seeded_config" = "1" ] && [ -n "$TOKEN" ]; then
    sed -i \
        -e 's|^[[:space:]]*read_only[[:space:]]*=.*|read_only = false|' \
        -e "s|^[[:space:]]*auth_token_file[[:space:]]*=.*|auth_token_file = \"${TOKEN_FILE}\"|" \
        /etc/easy-failover/config.toml || true
fi

# Bundled dashboard (present only when built with it): create user, seed env, hand it the
# write token, and wire a local-node roster entry. Binds localhost by default.
if [ -e /usr/lib/easy-failover-dashboard/server.js ]; then
    if ! getent passwd "$DASH_USER" >/dev/null 2>&1; then
        useradd --system --no-create-home --shell /sbin/nologin \
            --comment "easy-failover dashboard" "$DASH_USER" || true
    fi

    if [ ! -e "$DASH_ENV" ] \
        && [ -e /etc/easy-failover-dashboard/dashboard.env.example ]; then
        cp /etc/easy-failover-dashboard/dashboard.env.example "$DASH_ENV"
    fi
    if [ -e "$DASH_ENV" ]; then
        if [ -n "$TOKEN" ] \
            && ! grep -q '^[[:space:]]*EASY_FAILOVER_TOKEN_LOCAL=' "$DASH_ENV"; then
            printf 'EASY_FAILOVER_TOKEN_LOCAL=%s\n' "$TOKEN" >> "$DASH_ENV"
        fi
        chgrp "$DASH_USER" "$DASH_ENV" 2>/dev/null || true
        chmod 0640 "$DASH_ENV" || true
    fi

    mkdir -p "$DASH_STATE"
    chown "$DASH_USER":"$DASH_USER" "$DASH_STATE" 2>/dev/null || true
    chmod 0750 "$DASH_STATE" || true
    if [ ! -e "$DASH_ROSTER" ]; then
        cat > "$DASH_ROSTER" <<JSON
[
  {
    "id": "local",
    "label": "local",
    "apiBase": "http://127.0.0.1:8743",
    "tokenEnv": "EASY_FAILOVER_TOKEN_LOCAL"
  }
]
JSON
        chown "$DASH_USER":"$DASH_USER" "$DASH_ROSTER" 2>/dev/null || true
        chmod 0640 "$DASH_ROSTER" || true
    fi
fi

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || true
    # Auto-start the daemon; it idles until a VIP is configured (clean-slate config). Clear any
    # failed/start-limited state from a previous version, then enable (for boot) and start
    # separately so a tripped `enable` still leaves the unit running.
    systemctl reset-failed easy-failover.service 2>/dev/null || true
    systemctl enable easy-failover.service 2>/dev/null || true
    systemctl start easy-failover.service || true
    if [ -e /usr/lib/easy-failover-dashboard/server.js ]; then
        systemctl reset-failed easy-failover-dashboard.service 2>/dev/null || true
        systemctl enable easy-failover-dashboard.service 2>/dev/null || true
        systemctl start easy-failover-dashboard.service || true
    fi
fi
exit 0
