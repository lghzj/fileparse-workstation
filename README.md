# fileparse-workstation

文件解析平台工作站客户端仓库。工作站只负责监听本地设备输出目录、判断文件稳定、上传文件、接收 WebSocket 配置和解析结果，不执行服务端插件解析。

## 版本线

本仓库包含两个工作站实现：

- `python/`：当前正式交付版，基于 Python、PySide 和 PyInstaller，支持现代 Windows 和 Windows 7 独立构建链。
- `cpp-qt/`：C++/Qt 新版原型，保持自包含，后续可逐步替代 Python 版。

两个版本共享同一套服务端 HTTP API、WebSocket 消息和本地配置语义，但不依赖服务端源码。

## Python 正式版

复制并编辑配置：

```bash
cp python/workstation/server.example.json python/workstation/server.json
```

注册工作站：

```bash
uv run python -m workstation.cli --server python/workstation/server.json register
```

启动 GUI：

```bash
uv run python -m workstation.cli --server python/workstation/server.json --state-db python/workstation/state.db gui
```

启动后台监听：

```bash
uv run python -m workstation.cli --server python/workstation/server.json --state-db python/workstation/state.db run
```

构建现代 Windows 包：

```powershell
scripts\build_workstation.ps1
```

构建 Windows 7 包：

```powershell
scripts\build_workstation_win7.ps1
```

Windows 7 构建依赖 Python 3.8 x64，依赖锁定在 `python/workstation/requirements-win7.txt`。

## C++/Qt 原型版

```bash
cd cpp-qt
cmake -S . -B build
cmake --build build
```

平台打包脚本位于：

- `cpp-qt/packaging/windows/build-zip.ps1`
- `cpp-qt/packaging/linux/build-deb.sh`
- `cpp-qt/packaging/macos/package.sh`

## 仓库边界

- 不 import `fileparse-server` 的 Python 包或服务端源码。
- 不依赖 `fileparse-plugin-dev-kit`。
- 只通过服务端发布的 API 文档、OpenAPI 和 WebSocket 协议对齐。
