# RPM %post for easy-failover-dashboard: create the user, seed env, hand it the daemon's write
# token, wire a local-node roster entry, and start the service. Requires easy-failover, so the
# daemon (and its token) is configured first.
TOKEN_FILE=/etc/easy-failover/api.token
DASH_USER=easy-failover-dashboard
DASH_ENV=/etc/easy-failover-dashboard/dashboard.env
DASH_STATE=/var/lib/easy-failover-dashboard
DASH_ROSTER="${DASH_STATE}/nodes.json"

if ! getent passwd "$DASH_USER" >/dev/null 2>&1; then
    useradd --system --no-create-home --shell /sbin/nologin \
        --comment "easy-failover dashboard" "$DASH_USER" || true
fi

if [ ! -e "$DASH_ENV" ] \
    && [ -e /etc/easy-failover-dashboard/dashboard.env.example ]; then
    cp /etc/easy-failover-dashboard/dashboard.env.example "$DASH_ENV"
fi
TOKEN="$(cat "$TOKEN_FILE" 2>/dev/null || true)"
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

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || true
    systemctl reset-failed easy-failover-dashboard.service 2>/dev/null || true
    systemctl enable easy-failover-dashboard.service 2>/dev/null || true
    systemctl start easy-failover-dashboard.service || true
fi
exit 0
