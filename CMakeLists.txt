cmake_minimum_required(VERSION 3.24)

project(
    easy-failover
    VERSION 0.1.0
    DESCRIPTION "Lightweight C++ Linux agent for automatic virtual IP failover between nodes"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
include(CTest)
include(GNUInstallDirs)

set(
    EASY_FAILOVER_SYSTEMD_UNIT_DIR
    "lib/systemd/system"
    CACHE STRING
          "Install directory for systemd unit files, relative to CMAKE_INSTALL_PREFIX")
if(IS_ABSOLUTE "${EASY_FAILOVER_SYSTEMD_UNIT_DIR}")
    message(FATAL_ERROR "EASY_FAILOVER_SYSTEMD_UNIT_DIR must be relative to CMAKE_INSTALL_PREFIX")
endif()

configure_file(
    cmake/install-systemd-unit.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/install-systemd-unit.cmake"
    @ONLY)

set(
    EASY_FAILOVER_OPENRC_DIR
    "${CMAKE_INSTALL_SYSCONFDIR}/init.d"
    CACHE STRING
          "Install directory for the OpenRC init script")

configure_file(
    cmake/install-openrc.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/install-openrc.cmake"
    @ONLY)

set(
    EASY_FAILOVER_RUNIT_DIR
    "${CMAKE_INSTALL_SYSCONFDIR}/sv"
    CACHE STRING
          "Install directory for the runit service directory")

configure_file(
    cmake/install-runit.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/install-runit.cmake"
    @ONLY)

set(
    EASY_FAILOVER_DINIT_DIR
    "${CMAKE_INSTALL_SYSCONFDIR}/dinit.d"
    CACHE STRING
          "Install directory for the dinit service description")

configure_file(
    cmake/install-dinit.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/install-dinit.cmake"
    @ONLY)

set(
    EASY_FAILOVER_S6_DIR
    "${CMAKE_INSTALL_SYSCONFDIR}/s6"
    CACHE STRING
          "Install directory for the s6 service directory")

configure_file(
    cmake/install-s6.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/install-s6.cmake"
    @ONLY)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE)

FetchContent_Declare(
    CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.6.2
    GIT_SHALLOW TRUE)

FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(spdlog CLI11 tomlplusplus)

function(easy_failover_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            ${target}
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
endfunction()

add_library(
    easy-failover-core STATIC
    src/api/LocalApi.cpp
    src/config/Config.cpp
    src/core/Election.cpp
    src/core/FailoverDecision.cpp
    src/core/FailoverNode.cpp
    src/health/HealthCheck.cpp
    src/heartbeat/HeartbeatLoop.cpp
    src/heartbeat/HeartbeatMessage.cpp
    src/heartbeat/HeartbeatTransport.cpp
    src/heartbeat/UdpHeartbeatTransport.cpp
    src/platform/NetworkCommandRunner.cpp
    src/platform/linux/LinuxHealthCommandRunner.cpp
    src/platform/linux/LinuxHostname.cpp
    src/platform/linux/LinuxNetworkCommandRunner.cpp
    src/platform/linux/LinuxVipOwnershipProbe.cpp
    src/platform/linux/LinuxVipManager.cpp
    src/runtime/DaemonRuntime.cpp
    src/runtime/RuntimeLog.cpp
    src/runtime/ShutdownSignal.cpp)

target_include_directories(easy-failover-core PUBLIC src)

target_link_libraries(
    easy-failover-core
    PUBLIC spdlog::spdlog tomlplusplus::tomlplusplus)

if(BUILD_TESTING)
    target_compile_definitions(easy-failover-core PRIVATE EASY_FAILOVER_BUILD_TESTING)
endif()

easy_failover_target_warnings(easy-failover-core)

add_executable(easy-failover src/main.cpp)

target_link_libraries(
    easy-failover
    PRIVATE CLI11::CLI11 easy-failover-core)

easy_failover_target_warnings(easy-failover)

if(BUILD_TESTING)
    add_executable(easy-failover-tests tests/core_tests.cpp)
    target_link_libraries(easy-failover-tests PRIVATE easy-failover-core)
    target_compile_definitions(easy-failover-tests PRIVATE EASY_FAILOVER_BUILD_TESTING)
    easy_failover_target_warnings(easy-failover-tests)

    add_test(NAME easy-failover-core-tests COMMAND easy-failover-tests)
    set_tests_properties(easy-failover-core-tests PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# Install components: "daemon" is everything the .deb/.rpm ships (binary, example config, systemd
# unit, docs). The OpenRC/runit/dinit/s6 service files are their own components so packages exclude
# them (they target distros that don't use systemd), while a plain `cmake --install` still installs
# everything for source installs.
install(TARGETS easy-failover RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT daemon)

install(
    FILES configs/easy-failover.toml
    DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/easy-failover"
    RENAME config.example.toml
    COMPONENT daemon)

install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/cmake/install-systemd-unit.cmake" COMPONENT daemon)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/cmake/install-openrc.cmake" COMPONENT openrc)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/cmake/install-runit.cmake" COMPONENT runit)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/cmake/install-dinit.cmake" COMPONENT dinit)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/cmake/install-s6.cmake" COMPONENT s6)

install(FILES README.md LICENSE DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT daemon)
install(
    DIRECTORY docs/
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/docs"
    COMPONENT daemon
    FILES_MATCHING
    PATTERN "*.md")

# --- Native packaging (CPack -> .deb and .rpm) -------------------------------------------------
# Build packages with the package-oriented prefix:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
#         -DCMAKE_INSTALL_SYSCONFDIR=/etc
#   cmake --build build && (cd build && cpack)
# scripts/package.sh wraps this. .deb is built/verified locally; .rpm requires rpmbuild (CI/Fedora).
set(CPACK_PACKAGE_NAME "easy-failover")
set(CPACK_PACKAGE_VENDOR "EEkebin")
set(CPACK_PACKAGE_CONTACT "EEkebin <easy-failover@users.noreply.github.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/EEkebin/easy-failover")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

# Package only the daemon component as a single package; the alternative init-system files
# (openrc/runit/dinit/s6 components) stay out. Component-install mode is required for this filter
# to apply, with ALL_COMPONENTS_IN_ONE grouping so it stays one package, not per-component.
set(CPACK_COMPONENTS_ALL daemon)
set(CPACK_COMPONENTS_GROUPING "ALL_COMPONENTS_IN_ONE")
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_RPM_COMPONENT_INSTALL ON)
# Without a main component the RPM generator suffixes the package name/file with the group name
# (easy-failover-ALL_COMPONENTS_IN_ONE-...). Naming the main component keeps it "easy-failover".
set(CPACK_RPM_MAIN_COMPONENT daemon)

set(CPACK_GENERATOR "DEB;RPM")

# Debian/Ubuntu
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_PACKAGE_SECTION "admin")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "iproute2, arping")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/postinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/prerm;${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/postrm")

# Fedora/RHEL/Rocky (built where rpmbuild is available)
set(CPACK_RPM_PACKAGE_LICENSE "Apache-2.0")
set(CPACK_RPM_PACKAGE_GROUP "Applications/System")
set(CPACK_RPM_PACKAGE_REQUIRES "iproute, iputils")
set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/postinst.sh")
set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/prerm.sh")
set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/postrm.sh")
# Don't let the RPM claim ownership of system directories provided by other packages.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    "/etc;/usr/lib;/usr/lib/systemd;/usr/lib/systemd/system;/usr/share;/usr/share/doc")

include(CPack)
