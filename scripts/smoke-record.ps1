param(
    [string]$ScrcpyExe = "C:\Users\rokas\Downloads\scrcpy-win64-v4.0\scrcpy.exe",
    [ValidateSet("h264", "h265", "av1")]
    [string]$VideoCodec = "h264",
    [ValidateSet("opus", "aac", "flac", "raw")]
    [string]$AudioCodec = "opus",
    [string]$AudioBitRate = "128K",
    [string]$VideoBitRate = "8M",
    [int]$Seconds = 5,
    [string]$Output = ""
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $ScrcpyExe)) {
    throw "scrcpy executable not found: $ScrcpyExe"
}

if (-not $Output) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $Output = Join-Path (Get-Location) "smoke-$VideoCodec-$AudioCodec-$stamp.mkv"
}

& $ScrcpyExe `
    --no-control `
    --no-window `
    --video-codec=$VideoCodec `
    --video-bit-rate=$VideoBitRate `
    --audio-codec=$AudioCodec `
    --audio-bit-rate=$AudioBitRate `
    --record=$Output `
    --record-format=mkv `
    --time-limit=$Seconds

Get-Item -LiteralPath $Output

