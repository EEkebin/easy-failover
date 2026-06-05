#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

PREFIX="/usr"
SYSCONFDIR="/etc"
BUILD_DIR="build"
BUILD_TYPE="Release"
CREATE_CONFIG=1
RELOAD_SYSTEMD=1
DRY_RUN=0

usage() {
    cat <<'EOF'
Usage: scripts/install.sh [options]

Build and install easy-failover from source.

Options:
  --prefix PATH           Install prefix for binaries/docs/systemd unit (default: /usr)
  --sysconfdir PATH       Configuration directory root (default: /etc)
  --build-dir PATH        CMake build directory (default: build)
  --build-type TYPE       CMake build type (default: Release)
  --no-config             Do not create or validate config.toml
  --no-systemd-reload     Do not run systemctl daemon-reload
  --dry-run               Print commands without changing the host
  -h, --help              Show this help

The script never enables or starts easy-failover.service automatically.
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
        --build-dir)
            [[ $# -ge 2 ]] || die "--build-dir requires a path"
            BUILD_DIR="$2"
            shift 2
            ;;
        --build-type)
            [[ $# -ge 2 ]] || die "--build-type requires a value"
            BUILD_TYPE="$2"
            shift 2
            ;;
        --no-config)
            CREATE_CONFIG=0
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

require_command cmake
if [[ "${DRY_RUN}" -eq 0 && "${EUID}" -ne 0 ]]; then
    require_command sudo
fi

cd "${REPO_ROOT}"

CONFIG_DIR="${SYSCONFDIR%/}/easy-failover"
EXAMPLE_CONFIG="${CONFIG_DIR}/config.example.toml"
ACTIVE_CONFIG="${CONFIG_DIR}/config.toml"
BINARY_PATH="${PREFIX%/}/bin/easy-failover"

log "Building easy-failover (${BUILD_TYPE})"
run cmake -S . -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_INSTALL_SYSCONFDIR="${SYSCONFDIR}"
run cmake --build "${BUILD_DIR}"

log "Installing easy-failover under ${PREFIX}"
run_privileged cmake --install "${BUILD_DIR}" --prefix "${PREFIX}"

if [[ "${CREATE_CONFIG}" -eq 1 ]]; then
    if [[ "${DRY_RUN}" -eq 1 ]]; then
        log "Creating ${ACTIVE_CONFIG} from ${EXAMPLE_CONFIG} if it is missing"
        quote_command sudo install -d -m 0755 "${CONFIG_DIR}"
        printf 'test -f %q || ' "${ACTIVE_CONFIG}"
        quote_command sudo cp "${EXAMPLE_CONFIG}" "${ACTIVE_CONFIG}"
    else
        run_privileged install -d -m 0755 "${CONFIG_DIR}"
        if [[ -f "${ACTIVE_CONFIG}" ]]; then
            log "Keeping existing ${ACTIVE_CONFIG}"
        else
            [[ -f "${EXAMPLE_CONFIG}" ]] || die "installed example config not found: ${EXAMPLE_CONFIG}"
            run_privileged cp "${EXAMPLE_CONFIG}" "${ACTIVE_CONFIG}"
            log "Created ${ACTIVE_CONFIG}"
        fi
    fi

    log "Validating ${ACTIVE_CONFIG}"
    run "${BINARY_PATH}" --config "${ACTIVE_CONFIG}" --validate-config
fi

if [[ "${RELOAD_SYSTEMD}" -eq 1 ]]; then
    if command -v systemctl >/dev/null 2>&1; then
        log "Reloading systemd units"
        if ! run_privileged systemctl daemon-reload; then
            log "systemd reload failed; reload manually with: sudo systemctl daemon-reload"
        fi
    else
        log "systemctl not found; skipping systemd reload"
    fi
fi

if [[ "${CREATE_CONFIG}" -eq 1 ]]; then
    log "Install complete. Review ${ACTIVE_CONFIG}, then enable/start easy-failover.service when ready."
else
    log "Install complete. Create and validate config.toml, then enable/start easy-failover.service when ready."
fi
