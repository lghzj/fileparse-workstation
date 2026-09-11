# fileparse-workstation

文件解析平台工作站客户端仓库。工作站只负责监听本地设备输出路径、判断文件稳定、上传文件、接收 WebSocket 配置和解析结果，不执行服务端插件解析。

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

macOS Homebrew Qt 环境可直接使用预置配置：

```bash
cd cpp-qt
cmake --preset macos-homebrew
cmake --build --preset macos-homebrew
```

平台打包脚本位于：

- `cpp-qt/packaging/windows/build-zip.ps1`
- `cpp-qt/packaging/windows/build-win7-zip.ps1`
- `cpp-qt/packaging/linux/build-deb.sh`
- `cpp-qt/packaging/macos/package.sh`

构建 Qt Windows 7 包：

```powershell
scripts\build_qt_workstation_win7.ps1 -QtPrefix C:\Qt\5.15.2\msvc2019_64
```

Qt Windows 7 包必须使用 Qt 5.x，不能使用 Qt 6.x。脚本会生成 `cpp-qt/dist/windows-win7/NetStarWorkstation-Win7-x64.zip`，并随包带上 Qt SQLite/ODBC 插件。Access 数据库采集仍要求目标工作站安装 Microsoft Access Database Engine / ACE ODBC Driver，且驱动位数需要和工作站程序一致。

C++/Qt 版本当前覆盖注册、配置同步、WebSocket、目录监听、上传、本地 SQLite 状态、失败重试、托盘驻留和诊断导出。

### Windows 现场安装和验证

GitHub Actions 会生成两个工作站包：

- `NetStarWorkstation-windows-x64`：面向 Windows 10/11，使用 Qt 6 x64。
- `NetStarWorkstation-Win7-x64`：面向 Windows 7 SP1 x64，使用 Qt 5 x64。

下载 Actions artifact 后只需要解压一次，进入目录运行 `bin\netstar-workstation.exe`。如果下载得到的文件名没有 `.zip` 后缀，可手工补上 `.zip` 后再解压。

Access 采集依赖目标工作站本机安装 ACE ODBC 驱动：

- x64 工作站必须安装 `Microsoft Access Database Engine` x64 版本。
- 驱动名称需要包含 `Microsoft Access Driver (*.mdb, *.accdb)`。
- `.mdb` 和 `.accdb` 都通过同一个 Access ODBC 驱动读取。
- 如果机器已安装 32 位 Office/Access，安装 64 位 ACE 可能冲突；这种机器需要统一到 64 位 Office/ACE，或后续单独构建 x86 工作站。

Access 文件正常链路：监听到文件变化后，文件记录先显示 `转化中`；工作站复制快照并生成 `access_delta.json`；上传成功后进入 `解析中`；平台回传结果后显示 `解析完成` 或 `解析失败`。解析完成/失败会显示右下角自动关闭通知，5 秒后消失。

`重置` 按钮会停止工作站并删除本机解析记录、Access 增量文件、快照和日志；平台地址、MAC 和 Token 会保留。重置后同一个文件重新放入监听目录可以重新触发采集。

`watchPath` 支持两种配置：

- 文件夹路径：扫描目录下匹配 `fileType` 和 `watchFilePattern` 的文件。
- 具体文件路径：只监听这一份文件，适合固定 `.mdb` / `.accdb` 文件持续增长的场景。

## 仓库边界

- 不 import `fileparse-server` 的 Python 包或服务端源码。
- 不依赖 `fileparse-plugin-dev-kit`。
- 只通过服务端发布的 API 文档、OpenAPI 和 WebSocket 协议对齐。
