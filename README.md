# OBS scrcpy source plugin

Native OBS Studio input source for a read-only, headless scrcpy stream.

Target local runtime:

- OBS Studio: `C:\Program Files\obs-studio\bin\64bit\obs64.exe`
- scrcpy: `C:\Users\rokas\Downloads\scrcpy-win64-v4.0\scrcpy.exe`
- Verified device: `59081FDCH002B8` / Pixel 10 Pro

## Goal

Expose an OBS source that receives Android video/audio through scrcpy without showing a scrcpy window and without sending control events to the device.

Planned source options:

- Video codec: H.264, H.265, AV1
- Video encoder override
- Video bit rate
- Max size and max FPS
- Audio codec: Opus, AAC, FLAC, raw PCM
- Audio encoder override
- Audio bit rate
- Audio buffering
- Audio source
- USB or TCP/IP device selection
- Read-only mode, always enabled

## Current status

This repository contains a native OBS source plugin with a direct scrcpy 4.0 socket transport. It starts the scrcpy Android server in read-only mode, accepts the video/audio sockets through `adb reverse`, decodes packets with FFmpeg, and outputs raw frames/audio to OBS.

The plugin does not capture a scrcpy window. It runs the scrcpy server in read-only mode, receives encoded packets, decodes them with FFmpeg/libavcodec, then pushes raw frames and audio into OBS via `obs_source_output_video()` and `obs_source_output_audio()`.

When `Send frames to MLBB CoPilot CV bridge` is enabled, the decoded video path also publishes throttled BMP frames to `http://127.0.0.1:8787/api/capture/obs/frame`. This allows CoPilot to analyze the same source OBS is rendering without opening a second scrcpy session.

The project now follows the official `obs-plugintemplate` layout:

- `buildspec.json` contains plugin metadata.
- `CMakePresets.json` contains the Windows build preset.
- `cmake/` contains the OBS template CMake helpers.
- `data/locale/en-US.ini` contains plugin locale strings.
- The root `CMakeLists.txt` uses `find_package(libobs REQUIRED)` and `set_target_properties_plugin()`.

## Local checks

Run:

```powershell
.\scripts\validate-environment.ps1
.\scripts\probe-encoders.ps1
```

## Usage

1. Build and install with `.\scripts\configure-build.ps1 -Build`, then `.\scripts\install-user-plugin.ps1`.
2. Restart OBS Studio.
3. Add source: `scrcpy Device Source`.
4. Keep `scrcpy executable` pointed at `C:\Users\rokas\Downloads\scrcpy-win64-v4.0\scrcpy.exe`.
5. Select a connected ADB device from the device dropdown, or leave `Auto USB device`.
6. Use `Refresh devices` after plugging in/unplugging a phone.
7. Pick the video/audio codec and quality settings.
8. Enable `Send frames to MLBB CoPilot CV bridge` while the CoPilot backend is running to drive local CV from this native source.

The source is always read-only. It does not expose device controls.

The Pixel 10 Pro currently reports hardware video encoders for all three requested video codecs:

- H.264: `c2.google.avc.encoder`
- H.265: `c2.google.hevc.encoder`
- AV1: `c2.google.av1.encoder`

Audio encoders reported:

- Opus: `c2.android.opus.encoder`
- AAC: `c2.android.aac.encoder`
- FLAC: `c2.android.flac.encoder`

## Build requirements

The installed OBS runtime is present, but the normal OBS installer does not include C/C++ plugin headers such as `obs.h`.

Visual Studio Community 2026 is installed locally and includes MSVC plus CMake. The scripts use the bundled tools directly, so they do not need CMake or `cl.exe` on the regular PowerShell PATH.

Prepare the local OBS SDK shim:

```powershell
.\scripts\setup-obs-sdk.ps1
```

Configure only:

```powershell
.\scripts\configure-build.ps1
```

Configure and build:

```powershell
.\scripts\configure-build.ps1 -Build
```

The convenience script sets `OBS_PLUGIN_SKIP_DEPENDENCY_SETUP=ON` and points CMake at the local OBS 32.1.2 SDK shim generated from the installed OBS runtime. The project structure remains compatible with the official template flow for future CI/distribution work.

Install for the current Windows user:

```powershell
.\scripts\install-user-plugin.ps1
```

Install into the OBS application directory:

```powershell
.\scripts\install-global-plugin.ps1
```

This copies the DLL to:

```text
%APPDATA%\obs-studio\plugins\obs-scrcpy-source\bin\64bit
```

The locale/resources are installed from the official template-style `data` folder to:

```text
%APPDATA%\obs-studio\plugins\obs-scrcpy-source\data
```

It also copies the FFmpeg runtime DLLs from the scrcpy 4.0 package:

- `avcodec-62.dll`
- `avutil-60.dll`
- `swresample-6.dll`

## Install target

The user install script uses OBS's per-user plugin bundle layout:

```text
%APPDATA%\obs-studio\plugins\obs-scrcpy-source
```

The global install script copies the plugin DLL to:

```text
C:\Program Files\obs-studio\obs-plugins\64bit
```

and plugin data to:

```text
C:\Program Files\obs-studio\data\obs-plugins\obs-scrcpy-source
```
