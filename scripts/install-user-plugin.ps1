param(
    [string]$Configuration = "RelWithDebInfo",
    [string]$PluginName = "obs-scrcpy-source",
    [string]$FfmpegRuntimeDir = ""
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

$pluginRoot = Join-Path $env:APPDATA "obs-studio\plugins\$PluginName"
$binDir = Join-Path $pluginRoot "bin\64bit"
$dataDir = Join-Path $pluginRoot "data"

Remove-Item -LiteralPath $dataDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $binDir, $dataDir | Out-Null
Copy-Item -LiteralPath $dll -Destination (Join-Path $binDir "$PluginName.dll") -Force
Get-ChildItem -LiteralPath (Join-Path $repoRoot "data") |
    Copy-Item -Destination $dataDir -Recurse -Force

foreach ($runtimeDll in @("avcodec-62.dll", "avutil-60.dll", "swresample-6.dll")) {
    $source = Join-Path $FfmpegRuntimeDir $runtimeDll
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Required runtime DLL not found: $source"
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $binDir $runtimeDll) -Force
}

[pscustomobject]@{
    PluginRoot = $pluginRoot
    Dll = Join-Path $binDir "$PluginName.dll"
    Data = $dataDir
} | ConvertTo-Json
