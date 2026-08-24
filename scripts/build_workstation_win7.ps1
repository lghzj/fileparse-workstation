$ErrorActionPreference = "Stop"

Set-Location (Join-Path $PSScriptRoot "..")

$buildCommit = (git rev-parse --short=12 HEAD).Trim()
$buildTime = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
@"
BUILD_TARGET = "windows-7"
BUILD_COMMIT = "$buildCommit"
BUILD_TIME = "$buildTime"
"@ | Set-Content -Encoding UTF8 python\workstation\_build_info.py

$venvPath = Join-Path (Get-Location) ".venv-win7"
$pythonPath = Join-Path $venvPath "Scripts\python.exe"

if (-not (Test-Path $pythonPath)) {
    $pythonVersion = python -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')"
    if ($pythonVersion -ne "3.8") {
        throw "Windows 7 package requires Python 3.8, current Python is $pythonVersion"
    }
    python -m venv $venvPath
}

& $pythonPath -m pip install --upgrade "pip<25" "setuptools<76" wheel
& $pythonPath -m pip install --requirement python\workstation\requirements-win7.txt

$env:PYINSTALLER_CONFIG_DIR = (Join-Path (Get-Location) ".pyinstaller-cache-win7")
& $pythonPath -m PyInstaller --clean --noconfirm python\workstation\workstation-win7.spec --distpath dist\workstation-win7 --workpath build\workstation-win7

Write-Host "Built dist/workstation-win7/NetStarFileParseWorkstation-Win7/"
