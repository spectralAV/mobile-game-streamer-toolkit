<#
.SYNOPSIS
    Builds the production installer and portable zip package for OBS scrcpy source plugin.

.DESCRIPTION
    Stages all required binaries and assets (plugin DLL, debug symbols, FFmpeg 8.0.1 runtime
    libraries with D3D11VA hardware acceleration, Apple AirPlay 2 receiver runtime, iPhone USB
    reverse tethering helper & libusb0 runtime, plugin localization data, and scrcpy 4.1 runtime
    with ADB), then compiles the Inno Setup .exe installer and generates a portable release .zip archive.

.PARAMETER Configuration
    Build configuration (RelWithDebInfo or Release). Default is RelWithDebInfo.

.PARAMETER Build
    Force a rebuild of the plugin prior to packaging.

.PARAMETER SkipInstaller
    Skip building the Inno Setup .exe installer.

.PARAMETER SkipZip
    Skip generating the portable .zip archive.

.PARAMETER OutputDir
    Custom output directory for release artifacts. Defaults to <repoRoot>\release.
#>

param(
    [ValidateSet("RelWithDebInfo", "Release", "Debug")]
    [string]$Configuration = "RelWithDebInfo",
    [switch]$Build,
    [switch]$SkipInstaller,
    [switch]$SkipZip,
    [string]$OutputDir = "",
    [string]$ScrcpyRuntimeDir = "",
    [string]$AirPlayRuntimeDir = "",
    [string]$FfmpegRuntimeDir = "",
    [string]$LibusbRuntimeDir = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$buildspecPath = Join-Path $repoRoot "buildspec.json"

if (-not (Test-Path -LiteralPath $buildspecPath)) {
    throw "buildspec.json not found: $buildspecPath"
}

$buildspec = Get-Content -LiteralPath $buildspecPath -Raw | ConvertFrom-Json
$productName = $buildspec.name
$productVersion = $buildspec.version
$displayName = $buildspec.displayName
$author = $buildspec.author
$website = $buildspec.website

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host " Building Package for: $displayName v$productVersion" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

if (-not $OutputDir) {
    $OutputDir = Join-Path $repoRoot "release"
}
New-Item -ItemType Directory -Force $OutputDir | Out-Null

$dllPath = Join-Path $repoRoot "build\$Configuration\$productName.dll"
$pdbPath = Join-Path $repoRoot "build\$Configuration\$productName.pdb"
$helperPath = Join-Path $repoRoot "build\$Configuration\obs-airplay-tether-helper.exe"
$helperPdbPath = Join-Path $repoRoot "build\$Configuration\obs-airplay-tether-helper.pdb"

if ($Build -or (-not (Test-Path -LiteralPath $dllPath)) -or (-not (Test-Path -LiteralPath $helperPath))) {
    Write-Host "`n[1/5] Compiling plugin and helper ($Configuration)..." -ForegroundColor Yellow
    & (Join-Path $PSScriptRoot "configure-build.ps1") -Build -Configuration $Configuration
} else {
    Write-Host "`n[1/5] Using existing build: $dllPath" -ForegroundColor Green
}

if (-not (Test-Path -LiteralPath $dllPath)) {
    throw "Plugin DLL not found after build: $dllPath"
}
if (-not (Test-Path -LiteralPath $helperPath)) {
    throw "AirPlay USB tether helper not found after build: $helperPath"
}

# Resolve runtime source directories
if (-not $FfmpegRuntimeDir) {
    $FfmpegRuntimeDir = Join-Path $repoRoot "vendor\ffmpeg-8.0.1-full_build-shared\bin"
}
if (-not (Test-Path -LiteralPath $FfmpegRuntimeDir)) {
    throw "FFmpeg runtime directory not found: $FfmpegRuntimeDir"
}

if (-not $AirPlayRuntimeDir) {
    $AirPlayRuntimeDir = Join-Path $repoRoot "vendor\airplay-runtime"
}

if (-not $LibusbRuntimeDir) {
    $LibusbRuntimeDir = Join-Path $repoRoot "vendor\libusb-win32\bin\amd64"
}

if (-not $ScrcpyRuntimeDir) {
    $vendorScrcpy = Join-Path $repoRoot "vendor\scrcpy-4.1"
    if (Test-Path -LiteralPath $vendorScrcpy) {
        $ScrcpyRuntimeDir = $vendorScrcpy
    } elseif (Test-Path -LiteralPath "C:\Program Files\scrcpy\4.1") {
        $ScrcpyRuntimeDir = "C:\Program Files\scrcpy\4.1"
    } else {
        throw "scrcpy 4.1 directory not found in vendor\scrcpy-4.1 or C:\Program Files\scrcpy\4.1"
    }
}

# Stage files
$stagingDir = Join-Path $repoRoot "build\installer-staging"
Write-Host "`n[2/5] Staging files into $stagingDir..." -ForegroundColor Yellow

Remove-Item -LiteralPath $stagingDir -Recurse -Force -ErrorAction SilentlyContinue
$stagingPlugins = Join-Path $stagingDir "obs-plugins\64bit"
$stagingData = Join-Path $stagingDir "data\obs-plugins\$productName"
$stagingScrcpy = Join-Path $stagingDir "scrcpy-4.1"

New-Item -ItemType Directory -Force $stagingPlugins, $stagingData, $stagingScrcpy | Out-Null

# 1. Plugin DLL, Helper EXE & Debug Symbols
Copy-Item -LiteralPath $dllPath -Destination (Join-Path $stagingPlugins "$productName.dll") -Force
if (Test-Path -LiteralPath $pdbPath) {
    Copy-Item -LiteralPath $pdbPath -Destination (Join-Path $stagingPlugins "$productName.pdb") -Force
}
Copy-Item -LiteralPath $helperPath -Destination (Join-Path $stagingPlugins "obs-airplay-tether-helper.exe") -Force
if (Test-Path -LiteralPath $helperPdbPath) {
    Copy-Item -LiteralPath $helperPdbPath -Destination (Join-Path $stagingPlugins "obs-airplay-tether-helper.pdb") -Force
}

# 2. libusb-win32 Runtime for iPhone USB Reverse Tethering
$libusbDll = Join-Path $LibusbRuntimeDir "libusb0.dll"
if (-not (Test-Path -LiteralPath $libusbDll)) {
    throw "Required libusb0.dll missing: $libusbDll"
}
Copy-Item -LiteralPath $libusbDll -Destination (Join-Path $stagingPlugins "libusb0.dll") -Force

# 3. Apple AirPlay Runtime DLLs (copied before full FFmpeg so full D3D11VA/multi-codec FFmpeg DLLs win)
if (Test-Path -LiteralPath $AirPlayRuntimeDir) {
    $obsoleteAirPlayDlls = @("msys-2.0.dll", "avcodec-58.dll", "avutil-56.dll", "swscale-5.dll")
    Get-ChildItem -LiteralPath $AirPlayRuntimeDir -Filter "*.dll" | Where-Object {
        $obsoleteAirPlayDlls -notcontains $_.Name
    } | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $stagingPlugins $_.Name) -Force
    }
}

# 4. FFmpeg 8.0.1 Full Shared Runtime DLLs (D3D11VA + full video/audio codec support + swscale-9)
foreach ($runtimeDll in @("avcodec-62.dll", "avutil-60.dll", "swresample-6.dll")) {
    $source = Join-Path $FfmpegRuntimeDir $runtimeDll
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Required FFmpeg DLL missing: $source"
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $stagingPlugins $runtimeDll) -Force
}
$swscaleSource = Join-Path $FfmpegRuntimeDir "swscale-9.dll"
if (Test-Path -LiteralPath $swscaleSource) {
    Copy-Item -LiteralPath $swscaleSource -Destination (Join-Path $stagingPlugins "swscale-9.dll") -Force
}

# 5. Plugin Data & Localization
$sourceDataDir = Join-Path $repoRoot "data"
if (Test-Path -LiteralPath $sourceDataDir) {
    Get-ChildItem -LiteralPath $sourceDataDir | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $stagingData -Recurse -Force
    }
}

# 6. scrcpy 4.1 Runtime & ADB package
Get-ChildItem -LiteralPath $ScrcpyRuntimeDir | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $stagingScrcpy -Recurse -Force
}

# 7. Include README / Quick Start in root of staging for zip
$instructionsPath = Join-Path $repoRoot "installer\install-instructions.txt"
if (Test-Path -LiteralPath $instructionsPath) {
    Copy-Item -LiteralPath $instructionsPath -Destination (Join-Path $stagingDir "README-INSTALL.txt") -Force
}

Write-Host "      Staging complete." -ForegroundColor Green

# Find Inno Setup compiler
$isccPath = $null
$commandISCC = Get-Command "ISCC.exe" -ErrorAction SilentlyContinue
if ($commandISCC) {
    $isccPath = $commandISCC.Source
} else {
    $candidatePaths = @(
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
        "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
        "C:\Program Files\Inno Setup 6\ISCC.exe"
    )
    foreach ($cand in $candidatePaths) {
        if (Test-Path -LiteralPath $cand) {
            $isccPath = $cand
            break
        }
    }
}

# Build Inno Setup Installer
$installerName = "$productName-$productVersion-windows-x64-Installer"
if (-not $SkipInstaller) {
    Write-Host "`n[3/5] Compiling Inno Setup Installer..." -ForegroundColor Yellow
    if (-not $isccPath) {
        throw "Inno Setup compiler (ISCC.exe) was not found. Install it with: winget install --id JRSoftware.InnoSetup -e"
    }

    $issScript = Join-Path $repoRoot "installer\obs-scrcpy-source.iss"
    if (-not (Test-Path -LiteralPath $issScript)) {
        throw "Inno Setup script not found: $issScript"
    }

    $iconPath = Join-Path $repoRoot "vendor\scrcpy-4.0\app\data\icon.ico"

    & $isccPath `
        "/DMyAppVersion=$productVersion" `
        "/DMyAppName=$displayName" `
        "/DMyAppPublisher=$author" `
        "/DMyAppURL=$website" `
        "/DMyAppId=$productName" `
        "/DStagingDir=$stagingDir" `
        "/DOutputDir=$OutputDir" `
        "/DOutputBaseFilename=$installerName" `
        "/DSetupIconFile=$iconPath" `
        $issScript

    if ($LASTEXITCODE -ne 0) {
        throw "ISCC compilation failed with code $LASTEXITCODE"
    }
    Write-Host "      Installer created successfully: $OutputDir\$installerName.exe" -ForegroundColor Green
} else {
    Write-Host "`n[3/5] Skipping Installer compilation (-SkipInstaller)." -ForegroundColor DarkGray
}

# Create Portable ZIP Package
$zipName = "$productName-$productVersion-windows-x64.zip"
$zipPath = Join-Path $OutputDir $zipName
if (-not $SkipZip) {
    Write-Host "`n[4/5] Creating Portable Release ZIP..." -ForegroundColor Yellow
    Remove-Item -LiteralPath $zipPath -Force -ErrorAction SilentlyContinue

    $itemsToZip = Get-ChildItem -Path $stagingDir | Select-Object -ExpandProperty FullName
    Compress-Archive -Path $itemsToZip -DestinationPath $zipPath -CompressionLevel Optimal -Force
    Write-Host "      Portable ZIP created successfully: $zipPath" -ForegroundColor Green
} else {
    Write-Host "`n[4/5] Skipping Portable ZIP creation (-SkipZip)." -ForegroundColor DarkGray
}

# Summary and Checksums
Write-Host "`n[5/5] Release Artifacts Summary & Checksums:" -ForegroundColor Yellow
Write-Host "----------------------------------------------------------------------------" -ForegroundColor DarkGray

$artifacts = @()
$installerExe = Join-Path $OutputDir "$installerName.exe"
if (Test-Path -LiteralPath $installerExe) {
    $artifacts += Get-Item -LiteralPath $installerExe
}
if (Test-Path -LiteralPath $zipPath) {
    $artifacts += Get-Item -LiteralPath $zipPath
}

$summary = foreach ($art in $artifacts) {
    $hash = (Get-FileHash -LiteralPath $art.FullName -Algorithm SHA256).Hash
    [PSCustomObject]@{
        Name = $art.Name
        SizeMB = [math]::Round($art.Length / 1MB, 2)
        SHA256 = $hash
    }
}

$summary | Format-Table -AutoSize
Write-Host "All installer and packaging artifacts ready in: $OutputDir" -ForegroundColor Green
