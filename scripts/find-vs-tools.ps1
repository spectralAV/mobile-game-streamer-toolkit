param(
    [string]$VsWhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $VsWhere)) {
    throw "vswhere.exe not found: $VsWhere"
}

$install = & $VsWhere `
    -latest `
    -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -format json | ConvertFrom-Json | Select-Object -First 1

if (-not $install) {
    throw "No Visual Studio installation with MSVC tools was found."
}

$vsRoot = $install.installationPath
$cmake = Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$vsDevCmd = Join-Path $vsRoot "Common7\Tools\VsDevCmd.bat"

$msvcRoot = Join-Path $vsRoot "VC\Tools\MSVC"
$msvcVersion = Get-ChildItem -Directory $msvcRoot |
    Sort-Object Name -Descending |
    Select-Object -First 1

if (-not $msvcVersion) {
    throw "MSVC tools folder not found under $msvcRoot"
}

$hostTools = Join-Path $msvcVersion.FullName "bin\HostX64\x64"

[pscustomobject]@{
    VisualStudioRoot = $vsRoot
    DisplayName = $install.displayName
    Version = $install.installationVersion
    VsDevCmd = $vsDevCmd
    CMake = $cmake
    Ninja = $ninja
    Cl = Join-Path $hostTools "cl.exe"
    Lib = Join-Path $hostTools "lib.exe"
    DumpBin = Join-Path $hostTools "dumpbin.exe"
} | ConvertTo-Json

