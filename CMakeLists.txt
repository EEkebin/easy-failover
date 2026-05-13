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
    src/config/Config.cpp
    src/core/Election.cpp
    src/core/FailoverNode.cpp
    src/platform/linux/LinuxHostname.cpp
    src/platform/linux/LinuxVipManager.cpp)

target_include_directories(easy-failover-core PUBLIC src)

target_link_libraries(
    easy-failover-core
    PUBLIC spdlog::spdlog tomlplusplus::tomlplusplus)

easy_failover_target_warnings(easy-failover-core)

add_executable(easy-failover src/main.cpp)

target_link_libraries(
    easy-failover
    PRIVATE CLI11::CLI11 easy-failover-core)

easy_failover_target_warnings(easy-failover)

if(BUILD_TESTING)
    add_executable(easy-failover-tests tests/core_tests.cpp)
    target_link_libraries(easy-failover-tests PRIVATE easy-failover-core)
    easy_failover_target_warnings(easy-failover-tests)

    add_test(NAME easy-failover-core-tests COMMAND easy-failover-tests)
    set_tests_properties(easy-failover-core-tests PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

install(TARGETS easy-failover RUNTIME DESTINATION bin)
