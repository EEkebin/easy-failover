#!/usr/bin/env bash
# Configure and build easy-failover for development.
#
# Thin wrapper around the CMake commands so you don't have to remember them.
# This builds in the working tree only; it does not install anything. For system
# install use the distro package (see scripts/package.sh) or `cmake --install`.

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="Debug"
RUN_TESTS=0

usage() {
    cat <<'EOF'
Usage: scripts/build.sh [options]

Options:
  --release        Build in Release mode (default: Debug)
  --tests          Run the test suite after building
  --dir PATH       Build directory (default: build)
  -h, --help       Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --release) BUILD_TYPE="Release"; shift ;;
        --tests) RUN_TESTS=1; shift ;;
        --dir) [[ $# -ge 2 ]] || { echo "error: --dir requires a path" >&2; exit 1; }; BUILD_DIR="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown option: $1" >&2; usage; exit 1 ;;
    esac
done

cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
cmake --build "${BUILD_DIR}" --parallel

if [[ "${RUN_TESTS}" -eq 1 ]]; then
    ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi

echo "Built ${BUILD_TYPE} in ${BUILD_DIR}/ (binary: ${BUILD_DIR}/easy-failover)"
