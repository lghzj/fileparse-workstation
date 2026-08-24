param(
    [string]$QtPrefix = $env:QT_PREFIX,
    [string]$Generator = "Ninja",
    [string]$BuildType = "Release"
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectDir = Resolve-Path (Join-Path $ScriptDir "../..")
$BuildDir = Join-Path $ProjectDir "build-windows"
$PackageRoot = Join-Path $ProjectDir "dist/windows"
$StageDir = Join-Path $PackageRoot "NetStarWorkstation"
$ArchivePath = Join-Path $PackageRoot "NetStarWorkstation-windows-x64.zip"

if (-not $QtPrefix) {
    $qtBin = Get-Command windeployqt -ErrorAction SilentlyContinue
    if ($qtBin) {
        $QtPrefix = Split-Path -Parent (Split-Path -Parent $qtBin.Source)
    }
}

if (-not $QtPrefix) {
    throw "QtPrefix is required. Pass -QtPrefix C:\Qt\6.x.x\msvc2022_64 or set QT_PREFIX."
}

$CMakeArgs = @(
    "-S", $ProjectDir,
    "-B", $BuildDir,
    "-G", $Generator,
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DCMAKE_PREFIX_PATH=$QtPrefix"
)

cmake @CMakeArgs
cmake --build $BuildDir --config $BuildType

if (Test-Path $StageDir) {
    Remove-Item $StageDir -Recurse -Force
}
New-Item -ItemType Directory -Path $StageDir | Out-Null

cmake --install $BuildDir --config $BuildType --prefix $StageDir

$ExePath = Join-Path $StageDir "bin/netstar-workstation.exe"
if (-not (Test-Path $ExePath)) {
    throw "Executable not found: $ExePath"
}

$WinDeployQt = Join-Path $QtPrefix "bin/windeployqt.exe"
if (-not (Test-Path $WinDeployQt)) {
    throw "windeployqt.exe not found under QtPrefix: $WinDeployQt"
}

& $WinDeployQt --release --no-translations $ExePath

if (Test-Path $ArchivePath) {
    Remove-Item $ArchivePath -Force
}
Compress-Archive -Path (Join-Path $StageDir "*") -DestinationPath $ArchivePath

Write-Host "Windows package generated: $ArchivePath"
