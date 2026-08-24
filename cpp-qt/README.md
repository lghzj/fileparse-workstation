# NetStar Qt Workstation Prototype

This is a new C++/Qt workstation client prototype. It is intentionally isolated from the existing Python service and legacy workstation code.

## Scope

The client only collects and uploads files. It does not parse files and does not run Python plugins.

Current prototype features:

- Save local server/token/workstation settings with `QSettings`.
- Register or refresh workstation identity through `/api/fileparse/workstations/register`.
- Pull device/watch-path config through `/api/fileparse/workstations/config`.
- Connect to `/file` WebSocket and send heartbeat messages.
- Automatically reconnect WebSocket with exponential backoff after disconnects.
- Show WebSocket connection state in the UI status area.
- Apply `config.full` messages from WebSocket.
- Poll configured watch directories, wait for stable files, and auto-upload matching files.
- Support recursive watch configs with `maxDepth` and a per-scan file limit.
- Upload one selected file through `/api/fileparse/files/upload`.
- Calculate SHA256 for upload de-duplication metadata.
- Store upload records in local SQLite.
- Recover interrupted uploads after restart.
- Retry failed uploads on a timer.
- Mark upload success/failure and WebSocket task results in SQLite.
- Show recent upload records in a table.
- Manually retry a selected upload record.
- Clear failed upload records.
- Stay resident in the system tray.
- Hide to tray when the main window is closed.
- Provide tray actions for show, hide, and quit.
- Keep upload/retry logic outside the UI in `UploadManager`.
- Parse runtime configuration into typed `RuntimeConfig` and `DeviceConfig` models.
- Cache the latest runtime config locally and restore it on startup.
- Validate settings before network operations.
- Redact workstation tokens from UI logs.
- Write runtime logs to the local app data directory.
- Export diagnostics with redacted settings, runtime config, upload records, and runtime logs.

Planned next modules:

- `.deb` packaging scripts.

## Diagnostics

The diagnostics export writes a timestamped bundle under the selected output directory. It includes:

- `settings.redacted.json`: server, mac, hostname, app version, and masked token.
- `runtime-config.json`: latest cached runtime config.
- `upload-records.json`: recent local upload records.
- `workstation.log`: runtime log file or a placeholder if no log exists yet.

If `zip` is available on the host system, the exporter also creates a `.zip` beside the bundle directory.

## Build

Qt 6, CMake, and a C++17 compiler are required.

```bash
cd cpp-qt
cmake -S . -B build
cmake --build build
```

Or use the checked-in preset:

```bash
cmake --preset macos-homebrew
cmake --build --preset macos-homebrew
```

On macOS with Homebrew's split Qt packages:

```bash
brew install cmake ninja pkg-config qtbase qtwebsockets qttools

cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH="/opt/homebrew/opt/qtbase;/opt/homebrew/opt/qttools" \
  -DQt6WebSockets_DIR=/opt/homebrew/opt/qtwebsockets/lib/cmake/Qt6WebSockets

cmake --build build
```

Create a macOS app bundle and DMG on Apple Silicon:

```bash
cd cpp-qt
./packaging/macos/package.sh
```

Outputs:

- `dist/macos/NetStar Parse Hub.app`
- `dist/macos/NetStar-Parse-Hub-macOS-arm64.dmg`

Create a Debian package on UOS/Kylin after a successful build:

```bash
cmake --build build --target package
```

## Platform Boundary

Keep this directory self-contained. Do not modify the Python workstation implementation or server code unless the API contract is explicitly changed later.
