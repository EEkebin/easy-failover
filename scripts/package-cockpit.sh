#!/usr/bin/env bash
# Build the easy-failover-cockpit package: a Cockpit plugin (static assets +
# manifest) that depends on the easy-failover package and on Cockpit.
#
# Cockpit plugins are not compiled — they are HTML/CSS/JS dropped under
# /usr/share/cockpit/<name>/ — so this is a self-contained, architecture-
# independent package built directly with dpkg-deb (and rpmbuild where present),
# independent of the C++/CMake build.
#
#   PKG_VERSION=<v> scripts/package-cockpit.sh
#
# Outputs into build-cockpit/: easy-failover-cockpit_<v>_all.deb and, when
# rpmbuild is available, easy-failover-cockpit-<v>-1.noarch.rpm.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${ROOT}/cockpit"
OUT="${ROOT}/build-cockpit"
PLUGIN_DIR="usr/share/cockpit/easy-failover"

VERSION="${PKG_VERSION:-$("${ROOT}/scripts/version.sh" "${GITHUB_REF:-}")}"
MAINTAINER="easy-failover maintainers <noreply@anthropic.com>"
DESC_SHORT="Cockpit plugin for easy-failover"

echo "==> easy-failover-cockpit ${VERSION}"
rm -rf "${OUT}"
mkdir -p "${OUT}"

# ---- Staging tree (shared by deb + rpm) ---------------------------------------
stage_plugin() {
    local dest="$1"
    mkdir -p "${dest}/${PLUGIN_DIR}"
    install -m 0644 "${SRC}/manifest.json"      "${dest}/${PLUGIN_DIR}/manifest.json"
    install -m 0644 "${SRC}/index.html"         "${dest}/${PLUGIN_DIR}/index.html"
    install -m 0644 "${SRC}/app.js"             "${dest}/${PLUGIN_DIR}/app.js"
    install -m 0644 "${SRC}/easy-failover.css"  "${dest}/${PLUGIN_DIR}/easy-failover.css"
}

# ---- .deb (Debian/Ubuntu) -----------------------------------------------------
if command -v dpkg-deb >/dev/null 2>&1; then
    echo "==> Building .deb"
    debroot="${OUT}/deb"
    stage_plugin "${debroot}"
    mkdir -p "${debroot}/DEBIAN"
    cat > "${debroot}/DEBIAN/control" <<EOF
Package: easy-failover-cockpit
Version: ${VERSION}
Architecture: all
Maintainer: ${MAINTAINER}
Section: admin
Priority: optional
Depends: easy-failover, cockpit, cockpit-bridge
Description: ${DESC_SHORT}
 A Cockpit module for managing an easy-failover node from the Cockpit web
 console. It shows the local node's failover status and lets an authenticated
 administrator set the virtual IP and restart the daemon, using Cockpit's own
 login and privilege escalation (no separate API token required).
EOF
    deb="${OUT}/easy-failover-cockpit_${VERSION}_all.deb"
    dpkg-deb --root-owner-group --build "${debroot}" "${deb}" >/dev/null
    echo "    -> ${deb}"
fi

# ---- .rpm (Fedora/RHEL) -------------------------------------------------------
if command -v rpmbuild >/dev/null 2>&1; then
    echo "==> Building .rpm"
    rpmtop="${OUT}/rpmbuild"
    mkdir -p "${rpmtop}"/{BUILD,RPMS,SOURCES,SPECS,BUILDROOT}
    buildroot="${rpmtop}/BUILDROOT/easy-failover-cockpit-${VERSION}"
    stage_plugin "${buildroot}"
    cat > "${rpmtop}/SPECS/easy-failover-cockpit.spec" <<EOF
Name:           easy-failover-cockpit
Version:        ${VERSION}
Release:        1
Summary:        ${DESC_SHORT}
License:        Apache-2.0
BuildArch:      noarch
Requires:       easy-failover
Requires:       cockpit
Requires:       cockpit-bridge

%description
A Cockpit module for managing an easy-failover node from the Cockpit web
console. It shows the local node's failover status and lets an authenticated
administrator set the virtual IP and restart the daemon, using Cockpit's own
login and privilege escalation (no separate API token required).

%files
%dir /usr/share/cockpit/easy-failover
/usr/share/cockpit/easy-failover/manifest.json
/usr/share/cockpit/easy-failover/index.html
/usr/share/cockpit/easy-failover/app.js
/usr/share/cockpit/easy-failover/easy-failover.css

%changelog
* Tue Jun 09 2026 ${MAINTAINER} - ${VERSION}-1
- Automated build.
EOF
    rpmbuild --define "_topdir ${rpmtop}" \
        --buildroot "${buildroot}" \
        -bb "${rpmtop}/SPECS/easy-failover-cockpit.spec" >/dev/null
    find "${rpmtop}/RPMS" -name '*.rpm' -exec cp -v {} "${OUT}/" \;
fi

echo "==> Done. Artifacts in ${OUT}:"
ls -1 "${OUT}"/*.deb "${OUT}"/*.rpm 2>/dev/null || true
