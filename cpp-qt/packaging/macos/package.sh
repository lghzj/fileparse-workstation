#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
DIST_DIR="${PROJECT_DIR}/dist/macos"
APP_PATH="${BUILD_DIR}/netstar-workstation.app"
INFO_PLIST_PATH="${BUILD_DIR}/Info.plist"
DMG_PATH="${BUILD_DIR}/netstar-workstation.dmg"

if ! command -v cmake >/dev/null 2>&1; then
    echo "cmake is required" >&2
    exit 1
fi

if ! command -v macdeployqt >/dev/null 2>&1; then
    echo "macdeployqt is required. Install qttools with Homebrew." >&2
    exit 1
fi

cmake --preset macos-homebrew
cmake --build --preset macos-homebrew --target clean
rm -rf "${APP_PATH}"
cmake --build --preset macos-homebrew

if [[ ! -d "${APP_PATH}" ]]; then
    echo "Expected app bundle not found: ${APP_PATH}" >&2
    exit 1
fi

if [[ ! -f "${INFO_PLIST_PATH}" ]]; then
    echo "Expected Info.plist not found: ${INFO_PLIST_PATH}" >&2
    exit 1
fi

cp "${INFO_PLIST_PATH}" "${APP_PATH}/Contents/Info.plist"

rm -rf "${DIST_DIR}"
mkdir -p "${DIST_DIR}"

ditto "${APP_PATH}" "${DIST_DIR}/NetStar Parse Hub.app"
macdeployqt "${DIST_DIR}/NetStar Parse Hub.app" -verbose=1

rm -f "${DMG_PATH}" "${DIST_DIR}/NetStar-Parse-Hub-macOS-arm64.dmg"
hdiutil create \
    -volname "NetStar Parse Hub" \
    -srcfolder "${DIST_DIR}/NetStar Parse Hub.app" \
    -ov \
    -format UDZO \
    "${DIST_DIR}/NetStar-Parse-Hub-macOS-arm64.dmg"

echo "App: ${DIST_DIR}/NetStar Parse Hub.app"
if [[ -f "${DIST_DIR}/NetStar-Parse-Hub-macOS-arm64.dmg" ]]; then
    echo "DMG: ${DIST_DIR}/NetStar-Parse-Hub-macOS-arm64.dmg"
else
    echo "DMG was not produced; app bundle is ready." >&2
fi
