param(
    [string]$ObsVersion = "32.1.2",
    [string]$ObsRoot = "C:\Program Files\obs-studio",
    [string]$VendorDir = "vendor"
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$vendorRoot = Join-Path $repoRoot $VendorDir
$sdkRoot = Join-Path $vendorRoot "obs-sdk-$ObsVersion"
$sourceRoot = Join-Path $vendorRoot "obs-studio-$ObsVersion"
$zipPath = Join-Path $vendorRoot "obs-studio-$ObsVersion.zip"
$obsDll = Join-Path $ObsRoot "bin\64bit\obs.dll"
$obsLib = Join-Path $sdkRoot "lib\obs.lib"
$defPath = Join-Path $sdkRoot "lib\obs.def"
$cmakeConfigDir = Join-Path $sdkRoot "lib\cmake\libobs"
$obsConfigHeader = Join-Path $sdkRoot "include\obsconfig.h"

New-Item -ItemType Directory -Force $vendorRoot, $sdkRoot, (Join-Path $sdkRoot "lib"), $cmakeConfigDir, (Join-Path $sdkRoot "include") | Out-Null

if (-not (Test-Path -LiteralPath $obsDll)) {
    throw "obs.dll not found: $obsDll"
}

if (-not (Test-Path -LiteralPath (Join-Path $sourceRoot "libobs\obs.h"))) {
    if (-not (Test-Path -LiteralPath $zipPath)) {
        $url = "https://github.com/obsproject/obs-studio/archive/refs/tags/$ObsVersion.zip"
        Write-Host "Downloading OBS Studio $ObsVersion source headers..."
        Invoke-WebRequest -Uri $url -OutFile $zipPath
    }

    $extractTemp = Join-Path $vendorRoot "obs-extract-$ObsVersion"
    Remove-Item -LiteralPath $extractTemp -Recurse -Force -ErrorAction SilentlyContinue
    Expand-Archive -LiteralPath $zipPath -DestinationPath $extractTemp -Force

    $expanded = Get-ChildItem -Directory $extractTemp | Select-Object -First 1
    if (-not $expanded) {
        throw "OBS source archive did not expand as expected."
    }

    Remove-Item -LiteralPath $sourceRoot -Recurse -Force -ErrorAction SilentlyContinue
    Move-Item -LiteralPath $expanded.FullName -Destination $sourceRoot
    Remove-Item -LiteralPath $extractTemp -Recurse -Force -ErrorAction SilentlyContinue
}

$tools = & (Join-Path $PSScriptRoot "find-vs-tools.ps1") | ConvertFrom-Json

if (-not (Test-Path -LiteralPath $obsLib)) {
    $dump = & $tools.DumpBin /exports $obsDll
    $names = foreach ($line in $dump) {
        if ($line -match "^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+([A-Za-z_][A-Za-z0-9_@?$]*)(\s*=.*)?\s*$") {
            $matches[1]
        }
    }

    if (-not $names -or $names.Count -eq 0) {
        throw "Could not parse exports from $obsDll"
    }

    @("LIBRARY obs.dll", "EXPORTS") + ($names | Sort-Object -Unique | ForEach-Object { "  $_" }) |
        Set-Content -LiteralPath $defPath -Encoding ASCII

    & $tools.Lib /machine:x64 /def:$defPath /out:$obsLib | Write-Host
}

@'
#pragma once

#define OBS_DATA_PATH "data"
#define OBS_PLUGIN_PATH "obs-plugins"
#define OBS_PLUGIN_DESTINATION "obs-plugins/64bit"

#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
'@ | Set-Content -LiteralPath $obsConfigHeader -Encoding ASCII

$sourceRootCmake = ($sourceRoot -replace '\\', '/')
$sdkRootCmake = ($sdkRoot -replace '\\', '/')
$obsLibCmake = ($obsLib -replace '\\', '/')

@"
include_guard(GLOBAL)

if(NOT TARGET OBS::libobs)
  add_library(OBS::libobs UNKNOWN IMPORTED)
  set_target_properties(
    OBS::libobs
    PROPERTIES
      IMPORTED_LOCATION "$obsLibCmake"
      INTERFACE_INCLUDE_DIRECTORIES "$sourceRootCmake/libobs;$sdkRootCmake/include"
  )
endif()
"@ | Set-Content -LiteralPath (Join-Path $cmakeConfigDir "libobsConfig.cmake") -Encoding ASCII

[pscustomobject]@{
    ObsVersion = $ObsVersion
    SourceRoot = $sourceRoot
    IncludeDir = Join-Path $sourceRoot "libobs"
    ImportLib = $obsLib
    ObsDll = $obsDll
    PackageDir = $cmakeConfigDir
} | ConvertTo-Json
