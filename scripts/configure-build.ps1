param(
    [string]$Configuration = "RelWithDebInfo",
    [string]$ObsVersion = "32.1.2",
    [switch]$Build
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$tools = & (Join-Path $PSScriptRoot "find-vs-tools.ps1") | ConvertFrom-Json
$sdk = & (Join-Path $PSScriptRoot "setup-obs-sdk.ps1") -ObsVersion $ObsVersion | ConvertFrom-Json
$ffmpeg = & (Join-Path $PSScriptRoot "setup-ffmpeg-sdk.ps1") | ConvertFrom-Json
$buildDir = Join-Path $repoRoot "build"
$cacheFile = Join-Path $buildDir "CMakeCache.txt"
if (Test-Path -LiteralPath $cacheFile) {
    $expectedSource = ($repoRoot.Path -replace '\\', '/').TrimEnd('/')
    $homeLine = Select-String -LiteralPath $cacheFile -Pattern "^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$" | Select-Object -First 1
    if ($homeLine) {
        $cachedSource = ($homeLine.Matches[0].Groups[1].Value -replace '\\', '/').TrimEnd('/')
        if ($cachedSource -ne $expectedSource) {
            Write-Host "Cleaning stale CMake cache from moved directory ($cachedSource -> $expectedSource)..." -ForegroundColor Yellow
            Remove-Item -LiteralPath $buildDir -Recurse -Force
        }
    }
}

& $tools.CMake `
    -S $repoRoot `
    -B $buildDir `
    -G "Visual Studio 18 2026" `
    -A x64 `
    -DOBS_PLUGIN_SKIP_DEPENDENCY_SETUP=ON `
    "-Dlibobs_DIR=$($sdk.PackageDir)" `
    "-DFFMPEG_INCLUDE_DIR=$($ffmpeg.IncludeDir)" `
    "-DAVCODEC_LIB=$($ffmpeg.AvcodecLib)" `
    "-DAVUTIL_LIB=$($ffmpeg.AvutilLib)" `
    "-DSWRESAMPLE_LIB=$($ffmpeg.SwresampleLib)" `
    "-DSCRCPY_PACKAGE_DIR=$($ffmpeg.ScrcpyDir)"
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE"
}

if ($Build) {
    & $tools.CMake --build $buildDir --config $Configuration
    if ($LASTEXITCODE -ne 0) {
        throw "CMake build failed with exit code $LASTEXITCODE"
    }
}
