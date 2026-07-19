# OBS scrcpy source plugin

Native OBS Studio input source for a read-only, headless scrcpy stream.

Target local runtime:

- OBS Studio: `C:\Program Files\obs-studio\bin\64bit\obs64.exe`
- scrcpy: `C:\Users\rokas\Downloads\scrcpy-win64-v4.0\scrcpy.exe`
- Verified device: `59081FDCH002B8` / Pixel 10 Pro

## Goal

Expose OBS sources that receive device video/audio without showing external
capture windows:

- Android through scrcpy without sending control events to the device.
- Apple devices through AirPlay screen mirroring and device audio capture.

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

It also registers a separate `Apple AirPlay Source`. That source dynamically
loads `airplay2dll.dll` from AirPlayServer, starts an embedded AirPlay receiver,
and pushes decoded screen-mirroring video/audio callbacks into OBS. It does not
launch `AirPlayServer.exe` or capture a window.

The plugin does not capture a scrcpy window. It runs the scrcpy server in read-only mode, receives encoded packets, decodes them with FFmpeg/libavcodec, then pushes raw frames and audio into OBS via `obs_source_output_video()` and `obs_source_output_audio()`.

AirPlay receiver startup is deferred while OBS restores a scene collection.
Use **Restart receiver** in the AirPlay source properties to start it explicitly.
This prevents a failure inside the external AirPlay runtime from taking down OBS
during application startup.

When `Send frames to MLBB CoPilot CV bridge` is enabled, a background latest-frame worker publishes throttled raw BGRA samples to `http://127.0.0.1:8787/api/capture/obs/frame.raw`. This allows CoPilot to analyze the same source OBS is rendering without blocking OBS output or opening a second scrcpy session.

Video presentation is asynchronous and unbuffered in OBS for realtime streaming.
The plugin keeps the live edge without breaking decoder dependencies: when
measured lag becomes severe, it continues decoding compressed frames while
suppressing stale presentation. Presentation resumes as soon as the socket
backlog is drained, and the host clock is rebased so recovery cannot remain
latched indefinitely. It does not wait for a keyframe or restart the stream.
Video and audio share one scrcpy PTS clock for OBS sync. The configurable audio
delay is applied when scheduling OBS audio; the default is 0 ms.

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

1. Build and install with `.\scripts\configure-build.ps1 -Build`, then run
   `.\scripts\install-global-plugin.ps1` from an elevated PowerShell.
2. Restart OBS Studio.
3. Add source: `scrcpy Device Source`.
4. Keep `scrcpy executable` pointed at `C:\Users\rokas\Downloads\scrcpy-win64-v4.0\scrcpy.exe`.
5. Select a connected ADB device from the device dropdown, or leave `Auto USB device`.
6. Use `Refresh devices` after plugging in/unplugging a phone.
7. Pick the video/audio codec and quality settings:
   - **Video encoder** — dropdown from `Refresh encoders` / `scrcpy --list-encoders`. Leave on **Auto** for the device default, or pick a hardware encoder for your phone.
   - **MediaFormat encoder options** — separate from `--list-encoders`; optional keyframe interval presets. Leave on **Device default** unless a specific encoder requires an override.
   - **Encoder refresh** — default `0` (disabled); set a minute interval only if a particular device encoder accumulates latency over time.
   - **Audio buffer** — use `0` for lowest latency.
8. Enable `Send frames to MLBB CoPilot CV bridge` while the CoPilot backend is running to drive local CV from this native source.

The source is always read-only. It does not expose device controls.

If an ADB transport goes stale or the phone stream drops, the source reconnects
automatically with exponential backoff up to 60 seconds. Each attempt verifies
the selected device before deploying the server. It does not restart the global
ADB daemon, so another device or ADB client is not disrupted.

Encoder names vary by device. Run `scrcpy --list-encoders` with your phone connected. Examples:

- Pixel (Tensor): H.264 `c2.google.avc.encoder`, H.265 `c2.google.hevc.encoder`
- Pixel 4 (Qualcomm): H.264 `c2.qti.avc.encoder`, H.265 `c2.qti.hevc.encoder`

Audio encoders reported:

- Opus: `c2.android.opus.encoder`
- AAC: `c2.android.aac.encoder`
- FLAC: `c2.android.flac.encoder`

## Apple AirPlay source

1. Install Bonjour for Windows, or install iTunes if you prefer its bundled
   Bonjour service.
2. Build or download `airplay2dll.dll` from
   `https://github.com/xenos1337/AirPlayServer`.
3. Put `airplay2dll.dll` and its runtime DLLs next to
   `obs-scrcpy-source.dll`, or select `airplay2dll.dll` in the source settings.
4. Add source: `Apple AirPlay Source`.
5. Use the default receiver name `OBS AirPlay`, or set a custom name.
6. Open Screen Mirroring on the iPhone, iPad, or Mac and select the receiver.

The first AirPlay implementation supports screen mirroring plus device audio.
AirPlay media URL casting/playback is intentionally ignored.

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

This machine uses the global OBS plugin location as the authoritative install.
Do not also install the plugin per-user, because duplicate copies make it
unclear which DLL OBS loads.

Install globally from an elevated PowerShell (**close OBS first**, or the DLL will be locked):

```powershell
.\scripts\install-global-plugin.ps1
```

To also copy AirPlayServer runtime DLLs during install:

```powershell
.\scripts\install-global-plugin.ps1 -AirPlayRuntimeDir "C:\path\to\AirPlayServer\runtime"
```

This copies the DLL to:

```text
C:\Program Files\obs-studio\obs-plugins\64bit
```

The locale/resources are installed from the official template-style `data` folder to:

```text
C:\Program Files\obs-studio\data\obs-plugins\obs-scrcpy-source
```

It also copies the FFmpeg runtime DLLs from the full shared FFmpeg build under
`vendor\ffmpeg-8.0.1-full_build-shared\bin`. The smaller DLLs bundled with
scrcpy omit the D3D11VA decoder configuration required for hardware decoding.

- `avcodec-62.dll`
- `avutil-60.dll`
- `swresample-6.dll`

## Install target

The global install script copies the plugin DLL to:

```text
C:\Program Files\obs-studio\obs-plugins\64bit
```

and plugin data to:

```text
C:\Program Files\obs-studio\data\obs-plugins\obs-scrcpy-source
```
