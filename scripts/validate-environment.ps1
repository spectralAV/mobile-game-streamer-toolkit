param(
    [string]$ObsExe = "C:\Program Files\obs-studio\bin\64bit\obs64.exe",
    [string]$ScrcpyExe = "C:\Users\rokas\Downloads\scrcpy-win64-v4.0\scrcpy.exe"
)

$ErrorActionPreference = "Stop"

function Write-Check($Name, $Value) {
    if ($Value) {
        Write-Host "[ok] $Name`: $Value"
    } else {
        Write-Host "[missing] $Name"
    }
}

Write-Check "OBS executable" (Test-Path -LiteralPath $ObsExe)
Write-Check "scrcpy executable" (Test-Path -LiteralPath $ScrcpyExe)

if (Test-Path -LiteralPath $ObsExe) {
    & $ObsExe --version
}

if (Test-Path -LiteralPath $ScrcpyExe) {
    & $ScrcpyExe --version
}

Write-Check "CMake on PATH" (Get-Command cmake -ErrorAction SilentlyContinue).Source
Write-Check "MSVC cl.exe on PATH" (Get-Command cl -ErrorAction SilentlyContinue).Source
Write-Check "MSBuild on PATH" (Get-Command msbuild -ErrorAction SilentlyContinue).Source

$findVsTools = Join-Path $PSScriptRoot "find-vs-tools.ps1"
if (Test-Path -LiteralPath $findVsTools) {
    try {
        $tools = & $findVsTools | ConvertFrom-Json
        Write-Host "[ok] Visual Studio: $($tools.DisplayName) $($tools.Version)"
        Write-Check "Bundled CMake" $tools.CMake
        Write-Check "Bundled MSVC cl.exe" $tools.Cl
        Write-Check "Bundled lib.exe" $tools.Lib
    } catch {
        Write-Host "[missing] Visual Studio MSVC tools: $($_.Exception.Message)"
    }
}

$obsInclude = Get-ChildItem -Recurse -Filter obs.h "C:\Program Files\obs-studio" -ErrorAction SilentlyContinue | Select-Object -First 1
Write-Check "OBS headers in runtime install" $obsInclude.FullName
