# Support Guide

When opening a [GitHub Issue](https://github.com/spectralAV/mobile-game-streamer-toolkit/issues/new/choose), include environment details and sanitized diagnostic output so issues can be reproduced quickly.

## What To Include

1. **Plugin version** (e.g., `v0.3.7` installer, portable ZIP, or git commit SHA).
2. **Windows & OBS Studio version** (e.g., Windows 11 24H2, OBS Studio 32.1.2 x64).
3. **GPU model** (NVIDIA, AMD, or Intel — relevant for `D3D11VA` hardware decoding).
4. **Mobile device & OS version** (e.g., Pixel 10 Pro Android 16, iPhone 15 Pro iOS 18.4).
5. **Transport mode** (Android USB, Android TCP/IP, AirPlay Wi-Fi, or AirPlay + iPhone USB Reverse Tethering).

## Collecting Diagnostic Logs

- **OBS Studio Log**: In OBS Studio, open `Help -> Log Files -> View Current Log` and copy lines prefixed with `[obs-scrcpy-source]` or `[obs-airplay-source]`.
- **Installer Prerequisite Log**: `%TEMP%\obs-scrcpy-source-prereqs.log`
- **USB Reverse Tethering Helper Log**: `%APPDATA%\obs-studio\plugin_config\obs-scrcpy-source\tether_helper.log`

Quick PowerShell service check for AirPlay & iPhone USB Reverse Tethering:

```powershell
Get-Service "Bonjour Service", "Apple Mobile Device Service", "libusb0" -ErrorAction SilentlyContinue |
    Select-Object Name, Status, StartType
```

Quick Android ADB & `scrcpy` check:

```powershell
& "C:\Program Files\scrcpy\4.1\adb.exe" devices -l
& "C:\Program Files\scrcpy\4.1\scrcpy.exe" --version
```

## Privacy Reminder

Before pasting logs publicly, redact personal device serial numbers, iOS UDIDs, Wi-Fi BSSIDs, and private local file paths if sensitive.
