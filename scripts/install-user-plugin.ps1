param(
    [string]$Configuration = "RelWithDebInfo",
    [string]$PluginName = "obs-scrcpy-source",
    [string]$FfmpegRuntimeDir = "",
    [string]$AirPlayRuntimeDir = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$dll = Join-Path $repoRoot "build\$Configuration\$PluginName.dll"
if (-not $FfmpegRuntimeDir) {
    $FfmpegRuntimeDir = Join-Path $repoRoot "vendor\ffmpeg-8.0.1-full_build-shared\bin"
}
if (-not $AirPlayRuntimeDir) {
    $defaultAirPlay = Join-Path $repoRoot "vendor\airplay-runtime"
    if (Test-Path -LiteralPath $defaultAirPlay) {
        $AirPlayRuntimeDir = $defaultAirPlay
    }
}

Stop-Process -Name "obs64", "obs32" -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
$runningObs = Get-Process -Name "obs64", "obs32" -ErrorAction SilentlyContinue
if ($runningObs) {
    throw "OBS Studio is currently running (PID: $($runningObs.Id -join ', ')). Please close OBS Studio before installing the plugin."
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

$helperSource = Join-Path $repoRoot "build\$Configuration\obs-airplay-tether-helper.exe"
if (Test-Path -LiteralPath $helperSource) {
    Copy-Item -LiteralPath $helperSource -Destination (Join-Path $binDir "obs-airplay-tether-helper.exe") -Force
}
$libusbSource = Join-Path $repoRoot "vendor\libusb-win32\bin\amd64\libusb0.dll"
if (Test-Path -LiteralPath $libusbSource) {
    Copy-Item -LiteralPath $libusbSource -Destination (Join-Path $binDir "libusb0.dll") -Force
}

Get-ChildItem -LiteralPath (Join-Path $repoRoot "data") |
    Copy-Item -Destination $dataDir -Recurse -Force

if ($AirPlayRuntimeDir -and (Test-Path -LiteralPath $AirPlayRuntimeDir)) {
    foreach ($obsolete in @("msys-2.0.dll", "avcodec-58.dll", "avutil-56.dll", "swscale-5.dll")) {
        $obsoletePath = Join-Path $binDir $obsolete
        if (Test-Path -LiteralPath $obsoletePath) {
            Remove-Item -LiteralPath $obsoletePath -Force -ErrorAction SilentlyContinue
        }
    }
    Get-ChildItem -LiteralPath $AirPlayRuntimeDir -Filter "*.dll" | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $binDir $_.Name) -Force
    }
}

foreach ($runtimeDll in @("avcodec-62.dll", "avutil-60.dll", "swresample-6.dll")) {
    $source = Join-Path $FfmpegRuntimeDir $runtimeDll
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Required runtime DLL not found: $source"
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $binDir $runtimeDll) -Force
}
$swscaleSource = Join-Path $FfmpegRuntimeDir "swscale-9.dll"
if (Test-Path -LiteralPath $swscaleSource) {
    Copy-Item -LiteralPath $swscaleSource -Destination (Join-Path $binDir "swscale-9.dll") -Force
}

[pscustomobject]@{
    PluginRoot = $pluginRoot
    Dll = Join-Path $binDir "$PluginName.dll"
    Data = $dataDir
} | ConvertTo-Json
