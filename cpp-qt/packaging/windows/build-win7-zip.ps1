param(
    [string]$QtPrefix = $env:QT_PREFIX,
    [string]$Generator = "Ninja",
    [string]$BuildType = "Release",
    [string]$Arch = "x64",
    [string]$PackageName = "NetStarWorkstation-Win7"
)

$ErrorActionPreference = "Stop"

function Resolve-QtPrefix {
    param([string]$Prefix)

    if ($Prefix) {
        return (Resolve-Path $Prefix).Path
    }

    $qtBin = Get-Command windeployqt -ErrorAction SilentlyContinue
    if ($qtBin) {
        return (Split-Path -Parent (Split-Path -Parent $qtBin.Source))
    }

    throw "QtPrefix is required. Pass -QtPrefix C:\Qt\5.15.2\msvc2019_64 or set QT_PREFIX. Windows 7 packaging must use Qt 5.x."
}

function Get-QtVersion {
    param([string]$Prefix)

    $qmake = Join-Path $Prefix "bin\qmake.exe"
    if (-not (Test-Path $qmake)) {
        throw "qmake.exe not found under QtPrefix: $qmake"
    }

    return (& $qmake -query QT_VERSION).Trim()
}

function Require-Tool {
    param([string]$Name)

    $tool = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $tool) {
        throw "$Name was not found in PATH. Install it or run this script from a prepared developer shell."
    }
}

function Copy-QtSqlDriver {
    param(
        [string]$Prefix,
        [string]$StageDir,
        [string]$DriverName
    )

    $source = Join-Path $Prefix "plugins\sqldrivers\$DriverName.dll"
    if (-not (Test-Path $source)) {
        throw "Required Qt SQL driver not found: $source"
    }

    $targetDir = Join-Path $StageDir "bin\sqldrivers"
    New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
    Copy-Item $source $targetDir -Force
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectDir = (Resolve-Path (Join-Path $ScriptDir "../..")).Path
$QtPrefix = Resolve-QtPrefix $QtPrefix
$QtVersion = Get-QtVersion $QtPrefix

if (-not $QtVersion.StartsWith("5.")) {
    throw "Windows 7 package requires Qt 5.x. Current Qt at $QtPrefix is $QtVersion."
}

Require-Tool "cmake"

if ($Generator -eq "Ninja") {
    Require-Tool "ninja"
}

$BuildDir = Join-Path $ProjectDir "build-win7-$Arch"
$PackageRoot = Join-Path $ProjectDir "dist\windows-win7"
$StageDir = Join-Path $PackageRoot $PackageName
$ArchivePath = Join-Path $PackageRoot "$PackageName-$Arch.zip"
$BuildCommit = (& git -C $ProjectDir rev-parse --short=12 HEAD).Trim()
$BuildTime = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")

$CMakeArgs = @(
    "-S", $ProjectDir,
    "-B", $BuildDir,
    "-G", $Generator,
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DCMAKE_PREFIX_PATH=$QtPrefix",
    "-DCMAKE_SYSTEM_VERSION=6.1",
    "-DCMAKE_CXX_FLAGS=/D_WIN32_WINNT=0x0601 /DNTDDI_VERSION=0x06010000"
)

cmake @CMakeArgs
cmake --build $BuildDir --config $BuildType

if (Test-Path $StageDir) {
    Remove-Item $StageDir -Recurse -Force
}
New-Item -ItemType Directory -Path $StageDir | Out-Null

cmake --install $BuildDir --config $BuildType --prefix $StageDir

$ExePath = Join-Path $StageDir "bin\netstar-workstation.exe"
if (-not (Test-Path $ExePath)) {
    throw "Executable not found: $ExePath"
}

$WinDeployQt = Join-Path $QtPrefix "bin\windeployqt.exe"
if (-not (Test-Path $WinDeployQt)) {
    throw "windeployqt.exe not found under QtPrefix: $WinDeployQt"
}

& $WinDeployQt --release --compiler-runtime --no-translations --no-system-d3d-compiler --no-opengl-sw $ExePath

Copy-QtSqlDriver $QtPrefix $StageDir "qsqlite"
Copy-QtSqlDriver $QtPrefix $StageDir "qsqlodbc"

$linuxInstallDir = Join-Path $StageDir "share"
if (Test-Path $linuxInstallDir) {
    Remove-Item $linuxInstallDir -Recurse -Force
}

$BuildInfo = @{
    package = $PackageName
    target = "windows-7"
    arch = $Arch
    commit = $BuildCommit
    buildTime = $BuildTime
    qtVersion = $QtVersion
    qtPrefix = $QtPrefix
    accessDriver = "Microsoft Access Database Engine / ACE ODBC driver must be installed on the workstation"
}

$BuildInfo | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $StageDir "build-info.json")

if (Test-Path $ArchivePath) {
    Remove-Item $ArchivePath -Force
}
Compress-Archive -Path (Join-Path $StageDir "*") -DestinationPath $ArchivePath

Write-Host "Windows 7 Qt package generated: $ArchivePath"
Write-Host "Install Microsoft Access Database Engine / ACE ODBC driver on the target workstation before testing Access capture."
