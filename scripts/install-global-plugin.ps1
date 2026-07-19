param(
    [string]$Configuration = "RelWithDebInfo",
    [string]$PluginName = "obs-scrcpy-source",
    [string]$ObsRoot = "C:\Program Files\obs-studio",
    [string]$FfmpegRuntimeDir = "",
    [string]$AirPlayRuntimeDir = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$dll = Join-Path $repoRoot "build\$Configuration\$PluginName.dll"
if (-not $FfmpegRuntimeDir) {
    $FfmpegRuntimeDir = Join-Path $repoRoot "vendor\ffmpeg-8.0.1-full_build-shared\bin"
}

if (-not (Test-Path -LiteralPath $dll)) {
    throw "Plugin DLL not found. Build first with: .\scripts\configure-build.ps1 -Build"
}

$globalBinDir = Join-Path $ObsRoot "obs-plugins\64bit"
$globalDataDir = Join-Path $ObsRoot "data\obs-plugins\$PluginName"

if (-not (Test-Path -LiteralPath $globalBinDir)) {
    throw "OBS plugin directory not found: $globalBinDir"
}

if (-not (Test-Path -LiteralPath (Join-Path $ObsRoot "bin\64bit\obs64.exe"))) {
    throw "OBS executable not found under: $ObsRoot"
}

New-Item -ItemType Directory -Force $globalBinDir | Out-Null
Copy-Item -LiteralPath $dll -Destination (Join-Path $globalBinDir "$PluginName.dll") -Force

Remove-Item -LiteralPath $globalDataDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $globalDataDir | Out-Null
Get-ChildItem -LiteralPath (Join-Path $repoRoot "data") |
    Copy-Item -Destination $globalDataDir -Recurse -Force

foreach ($runtimeDll in @("avcodec-62.dll", "avutil-60.dll", "swresample-6.dll")) {
    $source = Join-Path $FfmpegRuntimeDir $runtimeDll
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Required runtime DLL not found: $source"
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $globalBinDir $runtimeDll) -Force
}

$airPlayRuntimeDlls = @()
if ($AirPlayRuntimeDir) {
    if (-not (Test-Path -LiteralPath $AirPlayRuntimeDir)) {
        throw "AirPlay runtime directory not found: $AirPlayRuntimeDir"
    }

    $airPlayDll = Join-Path $AirPlayRuntimeDir "airplay2dll.dll"
    if (-not (Test-Path -LiteralPath $airPlayDll)) {
        throw "airplay2dll.dll not found in AirPlay runtime directory: $AirPlayRuntimeDir"
    }

    Get-ChildItem -LiteralPath $AirPlayRuntimeDir -Filter "*.dll" |
        ForEach-Object {
            $target = Join-Path $globalBinDir $_.Name
            Copy-Item -LiteralPath $_.FullName -Destination $target -Force
            $airPlayRuntimeDlls += $target
        }
}

[pscustomobject]@{
    Dll = Join-Path $globalBinDir "$PluginName.dll"
    Data = $globalDataDir
    RuntimeDlls = @(
        Join-Path $globalBinDir "avcodec-62.dll"
        Join-Path $globalBinDir "avutil-60.dll"
        Join-Path $globalBinDir "swresample-6.dll"
    )
    AirPlayRuntimeDlls = $airPlayRuntimeDlls
} | ConvertTo-Json
