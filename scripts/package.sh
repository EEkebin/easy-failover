#!/usr/bin/env bash
# Build the native easy-failover package(s): .deb and, where rpmbuild is
# available, .rpm, via CPack.
#
# By default the package bundles BOTH the daemon and the web dashboard (a
# self-contained Next.js standalone build), so it needs Node.js + npm in
# addition to a C++ toolchain. Set EASY_FAILOVER_NO_DASHBOARD=1 for a
# daemon-only package (e.g. on architectures where the dashboard can't build).
#
# Uses the package-oriented prefix (/usr) and sysconfdir (/etc). The .deb
# generator needs dpkg-deb; the .rpm generator needs rpmbuild.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build-pkg}"

cmake_args=()

# Optional package version override (e.g. the rolling CalVer from scripts/version.sh).
if [ -n "${PKG_VERSION:-}" ]; then
    cmake_args+=("-DCPACK_PACKAGE_VERSION=${PKG_VERSION}")
fi

# Build and bundle the dashboard unless explicitly skipped.
if [ -z "${EASY_FAILOVER_NO_DASHBOARD:-}" ]; then
    web="${ROOT}/web"
    stage="${web}/dist-dashboard"
    echo "==> Building the bundled Next.js dashboard"
    (
        cd "${web}"
        npm ci
        # DASHBOARD_BUILD_FLAGS lets a caller pass extra `next build` flags (e.g. --webpack).
        # shellcheck disable=SC2086
        npm run build -- ${DASHBOARD_BUILD_FLAGS:-}
    )
    echo "==> Assembling the standalone runtime tree"
    rm -rf "${stage}"
    mkdir -p "${stage}"
    cp -r "${web}/.next/standalone/." "${stage}/"
    cp -r "${web}/.next/static" "${stage}/.next/static"
    if [ -d "${web}/public" ]; then
        cp -r "${web}/public" "${stage}/public"
    fi
    cmake_args+=("-DEASY_FAILOVER_DASHBOARD_DIST=${stage}")
else
    echo "==> EASY_FAILOVER_NO_DASHBOARD set; building a daemon-only package" >&2
fi

cmake -S "${ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_SYSCONFDIR=/etc \
    -DBUILD_TESTING=OFF \
    "${cmake_args[@]}"
cmake --build "${BUILD_DIR}" --parallel

# Pick generators by available tooling: .deb needs dpkg-deb, .rpm needs rpmbuild.
generators=""
if command -v dpkg-deb >/dev/null 2>&1; then
    generators="DEB"
fi
if command -v rpmbuild >/dev/null 2>&1; then
    generators="${generators:+${generators};}RPM"
fi
if [ -z "${generators}" ]; then
    echo "error: neither dpkg-deb nor rpmbuild found; cannot build any package" >&2
    exit 1
fi
echo "Building package generator(s): ${generators}" >&2

# Remove stale packages from previous runs so the produced artifact is unambiguous.
rm -f "${BUILD_DIR}"/*.deb "${BUILD_DIR}"/*.rpm

(cd "${BUILD_DIR}" && cpack -G "${generators}")

echo "Packages:"
ls -1 "${BUILD_DIR}"/*.deb "${BUILD_DIR}"/*.rpm 2>/dev/null || true
