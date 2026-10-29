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
$generator = "Visual Studio 17 2022"
if ($tools.Version -like "18.*") {
    $generator = "Visual Studio 18 2026"
} elseif ($tools.Version -like "17.*") {
    $generator = "Visual Studio 17 2022"
}

$cacheFile = Join-Path $buildDir "CMakeCache.txt"
if (Test-Path -LiteralPath $cacheFile) {
    $expectedSource = ($repoRoot.Path -replace '\\', '/').TrimEnd('/')
    $homeLine = Select-String -LiteralPath $cacheFile -Pattern "^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$" | Select-Object -First 1
    $genLine = Select-String -LiteralPath $cacheFile -Pattern "^CMAKE_GENERATOR:INTERNAL=(.+)$" | Select-Object -First 1
    $staleSource = $homeLine -and ((($homeLine.Matches[0].Groups[1].Value -replace '\\', '/').TrimEnd('/')) -ne $expectedSource)
    $staleGen = $genLine -and ($genLine.Matches[0].Groups[1].Value.Trim() -ne $generator)
    if ($staleSource -or $staleGen) {
        Write-Host "Cleaning stale CMake cache..." -ForegroundColor Yellow
        Remove-Item -LiteralPath $buildDir -Recurse -Force
    }
}

& $tools.CMake `
    -S $repoRoot `
    -B $buildDir `
    -G $generator `
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
