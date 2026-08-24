#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

build_commit="$(git rev-parse --short=12 HEAD)"
build_time="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
cat > python/workstation/_build_info.py <<EOF
BUILD_TARGET = "$(uname -s | tr '[:upper:]' '[:lower:]')"
BUILD_COMMIT = "$build_commit"
BUILD_TIME = "$build_time"
EOF

export PYINSTALLER_CONFIG_DIR="$PWD/.pyinstaller-cache"

uv sync --cache-dir .uv-cache --extra dev --extra gui
uv run --cache-dir .uv-cache pyinstaller --clean --noconfirm python/workstation/workstation.spec --distpath dist/workstation --workpath build/workstation

echo "Built dist/workstation/NetStarFileParseWorkstation/"
