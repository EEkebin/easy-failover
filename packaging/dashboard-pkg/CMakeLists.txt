# Standalone CMake project that packages the prebuilt Next.js dashboard into a
# .deb/.rpm via CPack. It compiles nothing (LANGUAGES NONE); it installs an
# already-assembled standalone tree. Build the tree and run CPack with
# scripts/package-dashboard.sh.

cmake_minimum_required(VERSION 3.24)

project(
    easy-failover-dashboard
    VERSION 0.1.0
    DESCRIPTION "Optional read-only web dashboard for easy-failover"
    LANGUAGES NONE)

include(GNUInstallDirs)

set(EASY_FAILOVER_DASHBOARD_DIST ""
    CACHE PATH "Path to the assembled Next.js standalone tree (must contain server.js)")
if(NOT EASY_FAILOVER_DASHBOARD_DIST OR NOT EXISTS "${EASY_FAILOVER_DASHBOARD_DIST}/server.js")
    message(FATAL_ERROR
        "EASY_FAILOVER_DASHBOARD_DIST must point at an assembled standalone tree containing "
        "server.js. Build it with scripts/package-dashboard.sh.")
endif()

# Arch-independent install location for the node app and tokens for the unit template.
set(EASY_FAILOVER_DASHBOARD_DIR "lib/easy-failover-dashboard")
set(EASY_FAILOVER_DASHBOARD_FULL_DIR "${CMAKE_INSTALL_PREFIX}/${EASY_FAILOVER_DASHBOARD_DIR}")
set(EASY_FAILOVER_DASHBOARD_SYSCONFDIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/../dashboard/easy-failover-dashboard.service.in"
    "${CMAKE_CURRENT_BINARY_DIR}/easy-failover-dashboard.service"
    @ONLY)

# The assembled standalone tree (server.js, node_modules, .next/...).
install(
    DIRECTORY "${EASY_FAILOVER_DASHBOARD_DIST}/"
    DESTINATION "${EASY_FAILOVER_DASHBOARD_DIR}"
    USE_SOURCE_PERMISSIONS)

install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/easy-failover-dashboard.service"
    DESTINATION "lib/systemd/system")

install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/../dashboard/easy-failover-dashboard.env.example"
    DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/easy-failover-dashboard"
    RENAME dashboard.env.example)

# --- CPack (.deb / .rpm) ---
set(CPACK_PACKAGE_NAME "easy-failover-dashboard")
set(CPACK_PACKAGE_VENDOR "EEkebin")
set(CPACK_PACKAGE_CONTACT "EEkebin <easy-failover@users.noreply.github.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/EEkebin/easy-failover")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

set(CPACK_GENERATOR "DEB;RPM")

# Debian/Ubuntu. Next.js 16 needs a modern Node; some distros require NodeSource.
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_PACKAGE_SECTION "admin")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "nodejs (>= 18.18.0)")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/../dashboard/deb/postinst;${CMAKE_CURRENT_SOURCE_DIR}/../dashboard/deb/prerm;${CMAKE_CURRENT_SOURCE_DIR}/../dashboard/deb/postrm")

# Fedora/RHEL/Rocky.
set(CPACK_RPM_PACKAGE_LICENSE "Apache-2.0")
set(CPACK_RPM_PACKAGE_GROUP "Applications/System")
set(CPACK_RPM_PACKAGE_REQUIRES "nodejs >= 1:18.18")
set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/../dashboard/rpm/postinst.sh")
set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/../dashboard/rpm/prerm.sh")
set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/../dashboard/rpm/postrm.sh")
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    "/etc;/usr/lib;/usr/lib/systemd;/usr/lib/systemd/system")

include(CPack)
