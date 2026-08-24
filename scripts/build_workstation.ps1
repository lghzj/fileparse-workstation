$ErrorActionPreference = "Stop"

Set-Location (Join-Path $PSScriptRoot "..")

$buildCommit = (git rev-parse --short=12 HEAD).Trim()
$buildTime = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
@"
BUILD_TARGET = "windows-modern"
BUILD_COMMIT = "$buildCommit"
BUILD_TIME = "$buildTime"
"@ | Set-Content -Encoding UTF8 python\workstation\_build_info.py

$env:PYINSTALLER_CONFIG_DIR = (Join-Path (Get-Location) ".pyinstaller-cache")

uv sync --cache-dir .uv-cache --extra dev --extra gui
uv run --cache-dir .uv-cache pyinstaller --clean --noconfirm python/workstation/workstation.spec --distpath dist/workstation --workpath build/workstation

Write-Host "Built dist/workstation/NetStarFileParseWorkstation/"
