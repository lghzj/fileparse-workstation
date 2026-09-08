param(
    [string]$QtPrefix = $env:QT_PREFIX,
    [string]$Generator = "Ninja",
    [string]$BuildType = "Release",
    [string]$Arch = "x64"
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$PackageScript = Join-Path $RepoRoot "cpp-qt\packaging\windows\build-win7-zip.ps1"

& $PackageScript -QtPrefix $QtPrefix -Generator $Generator -BuildType $BuildType -Arch $Arch
