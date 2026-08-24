#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build-uos"

cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" --target package

echo ""
echo "DEB package generated under: ${BUILD_DIR}"
find "${BUILD_DIR}" -maxdepth 1 -name '*.deb' -print
