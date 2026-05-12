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

add_executable(
    easy-failover
    src/main.cpp
    src/config/Config.cpp
    src/core/Election.cpp
    src/core/FailoverNode.cpp
    src/platform/linux/LinuxVipManager.cpp)

target_include_directories(easy-failover PRIVATE src)

target_link_libraries(
    easy-failover
    PRIVATE CLI11::CLI11 spdlog::spdlog tomlplusplus::tomlplusplus)

if(MSVC)
    target_compile_options(easy-failover PRIVATE /W4 /permissive-)
else()
    target_compile_options(
        easy-failover
        PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

install(TARGETS easy-failover RUNTIME DESTINATION bin)
