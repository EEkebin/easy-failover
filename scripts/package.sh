#!/usr/bin/env bash
# Build native packages (.deb and, where rpmbuild is available, .rpm) via CPack.
#
# Uses the package-oriented install prefix (/usr) and sysconfdir (/etc) so files
# land where a distro package expects. The .deb generator needs dpkg-deb; the
# .rpm generator needs rpmbuild (build the RPM on a Fedora/RHEL host or in CI if
# rpmbuild is not present here).

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build-pkg}"

cmake -S . -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_SYSCONFDIR=/etc \
    -DBUILD_TESTING=OFF
cmake --build "${BUILD_DIR}" --parallel

generators="DEB"
if command -v rpmbuild >/dev/null 2>&1; then
    generators="DEB;RPM"
else
    echo "note: rpmbuild not found; building .deb only (run on Fedora/RHEL or CI for .rpm)" >&2
fi

# Remove stale packages from previous runs so the produced artifact is unambiguous
# (the documented `apt install ./build-pkg/easy-failover_*.deb` glob assumes exactly one).
rm -f "${BUILD_DIR}"/*.deb "${BUILD_DIR}"/*.rpm

(cd "${BUILD_DIR}" && cpack -G "${generators}")

echo "Packages:"
ls -1 "${BUILD_DIR}"/*.deb "${BUILD_DIR}"/*.rpm 2>/dev/null || true
