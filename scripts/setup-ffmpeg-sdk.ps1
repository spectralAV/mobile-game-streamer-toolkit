param(
    [string]$FfmpegVersion = "8.0.1",
    [string]$ScrcpyDir = "C:\Program Files\scrcpy\4.1",
    [string]$VendorDir = "vendor"
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$vendorRoot = Join-Path $repoRoot $VendorDir
$sdkRoot = Join-Path $vendorRoot "ffmpeg-sdk-$FfmpegVersion"
$zipPath = Join-Path $vendorRoot "ffmpeg-$FfmpegVersion-full_build-shared.zip"
$sourceRoot = Join-Path $vendorRoot "ffmpeg-$FfmpegVersion-full_build-shared"
$runtimeBinRoot = Join-Path $sourceRoot "bin"
$libRoot = Join-Path $sdkRoot "lib"

New-Item -ItemType Directory -Force $vendorRoot, $sdkRoot, $libRoot | Out-Null

if (-not (Test-Path -LiteralPath (Join-Path $sourceRoot "include\libavcodec\avcodec.h"))) {
    if (-not (Test-Path -LiteralPath $zipPath)) {
        $url = "https://github.com/GyanD/codexffmpeg/releases/download/$FfmpegVersion/ffmpeg-$FfmpegVersion-full_build-shared.zip"
        Write-Host "Downloading FFmpeg $FfmpegVersion shared headers..."
        Invoke-WebRequest -Uri $url -OutFile $zipPath
    }

    $extractTemp = Join-Path $vendorRoot "ffmpeg-extract-$FfmpegVersion"
    Remove-Item -LiteralPath $extractTemp -Recurse -Force -ErrorAction SilentlyContinue
    Expand-Archive -LiteralPath $zipPath -DestinationPath $extractTemp -Force

    $expanded = Get-ChildItem -Directory $extractTemp | Select-Object -First 1
    if (-not $expanded) {
        throw "FFmpeg archive did not expand as expected."
    }

    Remove-Item -LiteralPath $sourceRoot -Recurse -Force -ErrorAction SilentlyContinue
    Move-Item -LiteralPath $expanded.FullName -Destination $sourceRoot
    Remove-Item -LiteralPath $extractTemp -Recurse -Force -ErrorAction SilentlyContinue
}

$tools = & (Join-Path $PSScriptRoot "find-vs-tools.ps1") | ConvertFrom-Json
$dlls = @("avcodec-62.dll", "avutil-60.dll", "swresample-6.dll")

foreach ($dllName in $dlls) {
    $dll = Join-Path $runtimeBinRoot $dllName
    if (-not (Test-Path -LiteralPath $dll)) {
        throw "FFmpeg DLL not found in full shared build: $dll"
    }

    $baseName = [System.IO.Path]::GetFileNameWithoutExtension($dllName)
    $defPath = Join-Path $libRoot "$baseName.def"
    $libPath = Join-Path $libRoot "$baseName.lib"

    $dump = & $tools.DumpBin /exports $dll
    $names = foreach ($line in $dump) {
        if ($line -match "^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+([A-Za-z_][A-Za-z0-9_@?$]*)(\s*=.*)?\s*$") {
            $matches[1]
        }
    }

    if (-not $names -or $names.Count -eq 0) {
        throw "Could not parse exports from $dll"
    }

    @("LIBRARY $dllName", "EXPORTS") + ($names | Sort-Object -Unique | ForEach-Object { "  $_" }) |
        Set-Content -LiteralPath $defPath -Encoding ASCII

    & $tools.Lib /machine:x64 /def:$defPath /out:$libPath | Write-Host
}

[pscustomObject]@{
    FfmpegVersion = $FfmpegVersion
    IncludeDir = Join-Path $sourceRoot "include"
    LibDir = $libRoot
    RuntimeBinDir = $runtimeBinRoot
    ScrcpyDir = $ScrcpyDir
    AvcodecLib = Join-Path $libRoot "avcodec-62.lib"
    AvutilLib = Join-Path $libRoot "avutil-60.lib"
    SwresampleLib = Join-Path $libRoot "swresample-6.lib"
} | ConvertTo-Json
