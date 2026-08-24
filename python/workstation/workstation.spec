# -*- mode: python ; coding: utf-8 -*-

from pathlib import Path

from PyInstaller.utils.hooks import collect_submodules

block_cipher = None
project_root = Path.cwd()
package_dir = project_root / "python" / "workstation"
windows_icon = project_root / "logo" / "workstation-icon-win.ico"

hiddenimports = collect_submodules("websockets") + [
    "PySide6.QtCore",
    "PySide6.QtGui",
    "PySide6.QtWidgets",
]

a = Analysis(
    [str(package_dir / "__main__.py")],
    pathex=[str(project_root / "python")],
    binaries=[],
    datas=[
        (str(package_dir / "server.example.json"), "."),
        (str(project_root / "logo" / "workstation-icon-mac-v2.png"), "logo"),
        (str(project_root / "logo" / "workstation-icon-win.ico"), "logo"),
    ],
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)
pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.zipfiles,
    a.datas,
    [],
    name="NetStarFileParseWorkstation",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    exclude_binaries=True,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=str(windows_icon) if windows_icon.exists() else None,
)

coll = COLLECT(
    exe,
    a.binaries,
    a.zipfiles,
    a.datas,
    strip=False,
    upx=True,
    upx_exclude=[],
    name="NetStarFileParseWorkstation",
)
