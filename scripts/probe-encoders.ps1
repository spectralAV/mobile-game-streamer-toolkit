param(
    [string]$ScrcpyExe = "C:\Users\rokas\Downloads\scrcpy-win64-v4.0\scrcpy.exe"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $ScrcpyExe)) {
    throw "scrcpy executable not found: $ScrcpyExe"
}

$ErrorActionPreference = "Continue"
$command = '"' + $ScrcpyExe + '" --list-encoders 2>&1'
$output = & cmd.exe /d /c $command
$exitCode = $LASTEXITCODE
$output

if ($exitCode -ne 0) {
    Write-Warning "scrcpy exited with code $exitCode after printing encoder information."
}
