#!/usr/bin/env bash
# Configure, build and test VikiCAD. Usage: scripts/build-and-test.sh [Debug|Release]
set -euo pipefail

BUILD_TYPE="${1:-Debug}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build/${BUILD_TYPE,,}"

GENERATOR=()
command -v ninja >/dev/null && GENERATOR=(-G Ninja)

cmake -S "$ROOT" -B "$BUILD_DIR" "${GENERATOR[@]}" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD_DIR" --parallel
ctest --test-dir "$BUILD_DIR" --output-on-failure
