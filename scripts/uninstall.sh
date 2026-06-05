#!/usr/bin/env bash
set -euo pipefail

PREFIX="/usr"
SYSCONFDIR="/etc"
SYSTEMD_UNIT_DIR="lib/systemd/system"
PURGE_CONFIG=0
RELOAD_SYSTEMD=1
DRY_RUN=0

usage() {
    cat <<'EOF'
Usage: scripts/uninstall.sh [options]

Remove a source-installed easy-failover installation.

Options:
  --prefix PATH              Install prefix for binaries/docs/systemd unit (default: /usr)
  --sysconfdir PATH          Configuration directory root (default: /etc)
  --systemd-unit-dir PATH    Unit directory relative to prefix (default: lib/systemd/system)
  --purge-config             Remove the active config directory too
  --no-systemd-reload        Do not run systemctl daemon-reload/reset-failed
  --dry-run                  Print commands without changing the host
  -h, --help                 Show this help

By default, config.toml is preserved. Use --purge-config only when you want to remove all local
easy-failover configuration.
EOF
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

log() {
    printf '%s\n' "$*"
}

quote_command() {
    local first=1
    local arg

    for arg in "$@"; do
        if [[ "${first}" -eq 0 ]]; then
            printf ' '
        fi
        printf '%q' "${arg}"
        first=0
    done
    printf '\n'
}

run() {
    if [[ "${DRY_RUN}" -eq 1 ]]; then
        quote_command "$@"
        return 0
    fi

    "$@"
}

run_privileged() {
    if [[ "${EUID}" -eq 0 ]]; then
        run "$@"
    else
        run sudo "$@"
    fi
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

remove_path() {
    local path="$1"

    if [[ "${DRY_RUN}" -eq 1 || -e "${path}" || -L "${path}" ]]; then
        run_privileged rm -rf "${path}"
    else
        log "Skipping missing ${path}"
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)
            [[ $# -ge 2 ]] || die "--prefix requires a path"
            PREFIX="$2"
            shift 2
            ;;
        --sysconfdir)
            [[ $# -ge 2 ]] || die "--sysconfdir requires a path"
            SYSCONFDIR="$2"
            shift 2
            ;;
        --systemd-unit-dir)
            [[ $# -ge 2 ]] || die "--systemd-unit-dir requires a path"
            SYSTEMD_UNIT_DIR="$2"
            shift 2
            ;;
        --purge-config)
            PURGE_CONFIG=1
            shift
            ;;
        --no-systemd-reload)
            RELOAD_SYSTEMD=0
            shift
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1"
            ;;
    esac
done

[[ "${PREFIX}" = /* ]] || die "--prefix must be an absolute path"
[[ "${SYSCONFDIR}" = /* ]] || die "--sysconfdir must be an absolute path"
[[ "${SYSTEMD_UNIT_DIR}" != /* ]] || die "--systemd-unit-dir must be relative to --prefix"

if [[ "${DRY_RUN}" -eq 0 && "${EUID}" -ne 0 ]]; then
    require_command sudo
fi

CONFIG_DIR="${SYSCONFDIR%/}/easy-failover"
EXAMPLE_CONFIG="${CONFIG_DIR}/config.example.toml"
ACTIVE_CONFIG="${CONFIG_DIR}/config.toml"
BINARY_PATH="${PREFIX%/}/bin/easy-failover"
DOC_DIR="${PREFIX%/}/share/doc/easy-failover"
SERVICE_UNIT="${PREFIX%/}/${SYSTEMD_UNIT_DIR%/}/easy-failover.service"

if command -v systemctl >/dev/null 2>&1; then
    log "Stopping and disabling easy-failover.service if systemd knows it"
    run_privileged systemctl disable --now easy-failover.service || true
else
    log "systemctl not found; skipping service stop/disable"
fi

log "Removing installed files"
remove_path "${SERVICE_UNIT}"
remove_path "${BINARY_PATH}"
remove_path "${DOC_DIR}"
remove_path "${EXAMPLE_CONFIG}"

if [[ "${PURGE_CONFIG}" -eq 1 ]]; then
    log "Purging ${CONFIG_DIR}"
    remove_path "${CONFIG_DIR}"
else
    if [[ "${DRY_RUN}" -eq 1 ]]; then
        printf 'if test -f %q; then printf %q\\\\n; fi\n' "${ACTIVE_CONFIG}" \
            "Keeping ${ACTIVE_CONFIG}; rerun with --purge-config to remove it."
    elif [[ -f "${ACTIVE_CONFIG}" ]]; then
        log "Keeping ${ACTIVE_CONFIG}; rerun with --purge-config to remove it."
    else
        run_privileged rmdir "${CONFIG_DIR}" 2>/dev/null || true
    fi
fi

if [[ "${RELOAD_SYSTEMD}" -eq 1 ]]; then
    if command -v systemctl >/dev/null 2>&1; then
        log "Reloading systemd units"
        run_privileged systemctl daemon-reload || true
        run_privileged systemctl reset-failed easy-failover.service || true
    else
        log "systemctl not found; skipping systemd reload"
    fi
fi

log "Uninstall complete."
log "If real VIP mutation was enabled, inspect and remove any remaining VIP manually."
