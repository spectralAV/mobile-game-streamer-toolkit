param(
    [string]$Configuration = "RelWithDebInfo",
    [string]$PluginName = "obs-scrcpy-source",
    [string]$ObsRoot = "C:\Program Files\obs-studio",
    [string]$FfmpegRuntimeDir = "",
    [string]$AirPlayRuntimeDir = ""
)

$ErrorActionPreference = "Stop"

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    try {
        Write-Host "Elevating with administrator privileges..."
        $argList = @("-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"")
        if ($Configuration) { $argList += @("-Configuration", "`"$Configuration`"") }
        $proc = Start-Process powershell -Verb RunAs -ArgumentList $argList -PassThru -Wait
        if ($proc.ExitCode -ne 0) {
            throw "Global plugin installation failed in elevated process (Exit code: $($proc.ExitCode))."
        }
        return
    } catch {
        Write-Warning "Could not automatically elevate ($($_.Exception.Message)). If this is an unelevated session, run PowerShell as Administrator."
    }
}

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

# Stop any running helper instances so files are not locked during installation
try {
    & schtasks /end /tn "OBS-AirPlay-Tether-Helper" 2>$null | Out-Null
} catch {}
Stop-Process -Name "obs-airplay-tether-helper" -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

if (-not (Test-Path -LiteralPath $dll)) {
    throw "Plugin DLL not found. Build first with: .\scripts\configure-build.ps1 -Build"
}

# Ensure scrcpy 4.1 is available at target location
$targetScrcpyExe = "C:\Program Files\scrcpy\4.1\scrcpy.exe"
if (-not (Test-Path -LiteralPath $targetScrcpyExe)) {
    $vendorScrcpy = Join-Path $repoRoot "vendor\scrcpy-4.1"
    if (Test-Path -LiteralPath $vendorScrcpy) {
        Write-Host "Installing scrcpy 4.1 to C:\Program Files\scrcpy\4.1..."
        New-Item -ItemType Directory -Force "C:\Program Files\scrcpy\4.1" | Out-Null
        Copy-Item -LiteralPath "$vendorScrcpy\*" -Destination "C:\Program Files\scrcpy\4.1" -Recurse -Force
    }
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

# Install USB Reverse Tethering Helper & register zero-UAC task
$helperSource = Join-Path $repoRoot "build\$Configuration\obs-airplay-tether-helper.exe"
if (Test-Path -LiteralPath $helperSource) {
    $helperTarget = Join-Path $globalBinDir "obs-airplay-tether-helper.exe"
    Copy-Item -LiteralPath $helperSource -Destination $helperTarget -Force

    try {
        $action = New-ScheduledTaskAction -Execute $helperTarget -Argument "--run"
        $principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Highest
        $task = New-ScheduledTask -Action $action -Principal $principal
        Register-ScheduledTask -TaskName "OBS-AirPlay-Tether-Helper" -InputObject $task -Force | Out-Null
        Write-Host "Registered elevated helper task: OBS-AirPlay-Tether-Helper"
    } catch {
        Write-Warning "Could not register scheduled task: $($_.Exception.Message)"
    }
}

# Permanently disable the iPhone PTP photo interface to eliminate the "Allow access to photos" prompt on plug-in
try {
    Get-PnpDevice | Where-Object { $_.InstanceId -like 'USB\VID_05AC&PID_12A*MI_00*' } | ForEach-Object {
        Write-Host "Disabling iPhone PTP photo interface to eliminate photo prompts: $($_.InstanceId)..."
        pnputil /disable-device "$($_.InstanceId)" | Out-Null
    }
} catch {}

$libusbSource = Join-Path $repoRoot "vendor\libusb-win32\bin\amd64\libusb0.dll"
if (Test-Path -LiteralPath $libusbSource) {
    Copy-Item -LiteralPath $libusbSource -Destination (Join-Path $globalBinDir "libusb0.dll") -Force
}

Remove-Item -LiteralPath $globalDataDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $globalDataDir | Out-Null
Get-ChildItem -LiteralPath (Join-Path $repoRoot "data") |
    Copy-Item -Destination $globalDataDir -Recurse -Force

$airPlayRuntimeDlls = @()
if ($AirPlayRuntimeDir) {
    if (-not (Test-Path -LiteralPath $AirPlayRuntimeDir)) {
        throw "AirPlay runtime directory not found: $AirPlayRuntimeDir"
    }

    $airPlayDll = Join-Path $AirPlayRuntimeDir "airplay2dll.dll"
    if (-not (Test-Path -LiteralPath $airPlayDll)) {
        throw "airplay2dll.dll not found in AirPlay runtime directory: $AirPlayRuntimeDir"
    }

    # Remove obsolete runtime DLLs from older AirPlay runtime versions (msys2, ffmpeg 4.x)
    foreach ($obsolete in @("msys-2.0.dll", "avcodec-58.dll", "avutil-56.dll", "swscale-5.dll")) {
        $obsoletePath = Join-Path $globalBinDir $obsolete
        if (Test-Path -LiteralPath $obsoletePath) {
            Remove-Item -LiteralPath $obsoletePath -Force -ErrorAction SilentlyContinue
        }
    }

    Get-ChildItem -LiteralPath $AirPlayRuntimeDir -Filter "*.dll" |
        ForEach-Object {
            $target = Join-Path $globalBinDir $_.Name
            Copy-Item -LiteralPath $_.FullName -Destination $target -Force
            $airPlayRuntimeDlls += $target
        }
}

# Copy full FFmpeg 8.0.1 runtime after AirPlay DLLs so full D3D11VA/multi-codec DLLs are never overwritten
foreach ($runtimeDll in @("avcodec-62.dll", "avutil-60.dll", "swresample-6.dll")) {
    $source = Join-Path $FfmpegRuntimeDir $runtimeDll
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Required runtime DLL not found: $source"
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $globalBinDir $runtimeDll) -Force
}
$swscaleSource = Join-Path $FfmpegRuntimeDir "swscale-9.dll"
if (Test-Path -LiteralPath $swscaleSource) {
    Copy-Item -LiteralPath $swscaleSource -Destination (Join-Path $globalBinDir "swscale-9.dll") -Force
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
