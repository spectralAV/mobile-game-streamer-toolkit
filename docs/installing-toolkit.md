# Installing Mobile Game Streamer Toolkit

This guide covers installing `obs-scrcpy-source` on Windows 10/11 (64-bit) with OBS Studio 30+ / 32+.

## 1. Standalone Windows Installer (Recommended)

1. Close **OBS Studio** (`obs64.exe`) and any standalone `scrcpy.exe` windows.
2. Download `obs-scrcpy-source-0.3.6-windows-x64-Installer.exe` from [GitHub Releases](https://github.com/spectralAV/mobile-game-streamer-toolkit/releases/latest).
3. Run the installer as Administrator:
   - The wizard automatically locates your OBS Studio installation directory (e.g., `C:\Program Files\obs-studio`).
   - Keep **Full Out-of-the-Box Installation** selected to install:
     - Core OBS plugin (`obs-scrcpy-source.dll`) + FFmpeg 8.0.1 `D3D11VA` hardware-decoding runtime
     - `scrcpy 4.1` runtime & Android `adb` tools (`C:\Program Files\scrcpy\4.1`)
     - Apple AirPlay 2 receiver runtime & USB Reverse Tethering helper
     - **Apple Bonjour Service v3.1** (skipped automatically if an equal/newer version is already installed)
     - **Apple Mobile Device Support v20.0 + WHQL USB/CDC-NCM drivers** (without iTunes player bloat)
     - **Windows Defender Firewall** inbound rules for OBS Studio & mDNS (`UDP 5353`)
4. Launch OBS Studio and add either **scrcpy Device Source** (Android) or **Apple AirPlay Source** (iPhone / iPad / Mac) from the **Sources (`+`)** menu.

## 2. Portable ZIP Installation

1. Download `obs-scrcpy-source-0.3.6-windows-x64.zip` from [GitHub Releases](https://github.com/spectralAV/mobile-game-streamer-toolkit/releases/latest).
2. Extract `obs-plugins\` and `data\` into your portable OBS Studio root folder.
3. Extract `scrcpy-4.1\` to `C:\Program Files\scrcpy\4.1` (or point the source's **scrcpy executable** path to your extracted `scrcpy.exe`).
4. Optional (for AirPlay & iPhone USB Reverse Tethering on a fresh PC): run `redist\setup-prerequisites.ps1` from an elevated PowerShell prompt to install Bonjour, Apple USB/NCM drivers, and firewall rules.

## 3. Building & Installing From Source

Prerequisites: Windows 10/11 x64, Visual Studio 2022 or 2026 with **Desktop development with C++**, and PowerShell.

```powershell
git clone https://github.com/spectralAV/mobile-game-streamer-toolkit.git
cd mobile-game-streamer-toolkit
.\scripts\configure-build.ps1 -Build -Configuration RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo --output-on-failure
.\scripts\install-global-plugin.ps1
```

## 4. First-Run Device Setup

### Android (`scrcpy Device Source`)
1. Enable **Developer options** -> **USB debugging** on your Android device.
2. Connect via USB data cable and tap **Allow USB debugging** on the phone screen.
3. Add **scrcpy Device Source** in OBS Studio -> click **Refresh devices** -> select your device.

### iPhone / iPad (`Apple AirPlay Source`)
1. Add **Apple AirPlay Source** in OBS Studio and enable the receiver (default name: `OBS AirPlay`).
2. **Wi-Fi / LAN mode**: Connect your iOS device to the same 5 GHz Wi-Fi / LAN as your PC, open **Control Center -> Screen Mirroring**, and select `OBS AirPlay`.
3. **Zero-Lag USB Cable mode**: Connect your iPhone via USB cable, unlock the screen, tap **Trust** if prompted, and check **Enable iPhone USB Reverse Tethering** in the AirPlay source properties. Once active, select `OBS AirPlay` in iOS Screen Mirroring. When you uncheck USB Reverse Tethering or close OBS, your iPhone automatically reverts to 100% stock Apple USB drivers.
