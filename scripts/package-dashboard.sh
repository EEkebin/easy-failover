#!/usr/bin/env bash
# Build the easy-failover dashboard package(s): .deb and, where rpmbuild is
# available, .rpm. Produces a Next.js standalone build, assembles the runnable
# tree, and packages it via CPack.
#
# Needs Node.js + npm (to build the dashboard) and dpkg-deb and/or rpmbuild.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WEB="${ROOT}/web"
STAGE="${WEB}/dist-dashboard"
BUILD_DIR="${BUILD_DIR:-${ROOT}/build-dashboard-pkg}"

echo "==> Building the Next.js standalone dashboard"
# DASHBOARD_BUILD_FLAGS lets a caller pass extra `next build` flags. Used on
# riscv64, where Turbopack (the Next 16 default) has no native bindings, to
# force the pure-JS webpack bundler: DASHBOARD_BUILD_FLAGS=--webpack.
(
    cd "${WEB}"
    npm ci
    # shellcheck disable=SC2086 # intentional word-splitting of the flags
    npm run build -- ${DASHBOARD_BUILD_FLAGS:-}
)

echo "==> Assembling the standalone runtime tree"
rm -rf "${STAGE}"
mkdir -p "${STAGE}"
cp -r "${WEB}/.next/standalone/." "${STAGE}/"
# Static assets are not part of the standalone output; copy them alongside.
cp -r "${WEB}/.next/static" "${STAGE}/.next/static"
# public/ is optional and absent in this app; copy it if it ever exists.
if [ -d "${WEB}/public" ]; then
    cp -r "${WEB}/public" "${STAGE}/public"
fi

echo "==> Packaging via CPack"
version_args=()
if [ -n "${PKG_VERSION:-}" ]; then
    version_args+=("-DCPACK_PACKAGE_VERSION=${PKG_VERSION}")
fi
cmake -S "${ROOT}/packaging/dashboard-pkg" -B "${BUILD_DIR}" \
    -DEASY_FAILOVER_DASHBOARD_DIST="${STAGE}" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_SYSCONFDIR=/etc \
    "${version_args[@]}"
cmake --build "${BUILD_DIR}"

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

rm -f "${BUILD_DIR}"/*.deb "${BUILD_DIR}"/*.rpm
(cd "${BUILD_DIR}" && cpack -G "${generators}")

echo "Packages:"
ls -1 "${BUILD_DIR}"/*.deb "${BUILD_DIR}"/*.rpm 2>/dev/null || true
