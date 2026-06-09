#!/usr/bin/env bash
# Build native packages (.deb and, where rpmbuild is available, .rpm) via CPack.
#
# Uses the package-oriented install prefix (/usr) and sysconfdir (/etc) so files
# land where a distro package expects. The .deb generator needs dpkg-deb; the
# .rpm generator needs rpmbuild (build the RPM on a Fedora/RHEL host or in CI if
# rpmbuild is not present here).

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build-pkg}"

# Optional package version override (e.g. the rolling CalVer from scripts/version.sh).
version_args=()
if [ -n "${PKG_VERSION:-}" ]; then
    version_args+=("-DCPACK_PACKAGE_VERSION=${PKG_VERSION}")
fi

cmake -S . -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_SYSCONFDIR=/etc \
    -DBUILD_TESTING=OFF \
    "${version_args[@]}"
cmake --build "${BUILD_DIR}" --parallel

# Pick generators by available tooling: .deb needs dpkg-deb, .rpm needs rpmbuild.
# This lets the same script build .deb on Debian/Ubuntu and .rpm on Fedora/RHEL.
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

# Remove stale packages from previous runs so the produced artifact is unambiguous
# (the documented `apt install ./build-pkg/easy-failover_*.deb` glob assumes exactly one).
rm -f "${BUILD_DIR}"/*.deb "${BUILD_DIR}"/*.rpm

(cd "${BUILD_DIR}" && cpack -G "${generators}")

echo "Packages:"
ls -1 "${BUILD_DIR}"/*.deb "${BUILD_DIR}"/*.rpm 2>/dev/null || true
