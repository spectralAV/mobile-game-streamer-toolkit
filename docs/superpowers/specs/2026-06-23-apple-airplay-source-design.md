# Apple AirPlay Source Design

Date: 2026-06-23

## Goal

Add wireless Apple device capture to the existing OBS plugin as a separate OBS
source. The new source captures screen mirroring video and device audio from
iPhone, iPad, and macOS AirPlay senders without launching an external AirPlay
window.

## Non-Goals

- Do not merge Apple support into the existing Android `scrcpy Device Source`.
- Do not support AirPlay media URL casting/playback in the first version.
- Do not add Apple device control, input injection, or remote-control features.
- Do not replace the current installed plugin backbone or global install model.

## Selected Approach

Embed the AirPlay receiver path from `xenos1337/AirPlayServer` and use its
decoded callback API as the bridge into OBS.

The plugin will add a second source named `Apple AirPlay Source` in the same
plugin DLL. The existing `scrcpy Device Source` remains Android-only. Shared
OBS media output helpers can be extracted so both runtimes use the same
source-frame and audio-frame conventions.

## Architecture

The plugin will contain two independent transport runtimes:

- `ScrcpyRuntime` keeps the current Android scrcpy transport.
- `AirPlayRuntime` owns the embedded AirPlay server lifetime and implements
  the AirPlay callback interface.

A small shared media-output layer will handle reusable OBS output behavior:

- YUV420/I420 video plane mapping to `obs_source_output_video()`.
- PCM audio metadata conversion to `obs_source_output_audio()`.
- Width and height tracking for OBS source sizing.
- Optional future hook point for the existing CV bridge, without making the
  bridge part of AirPlay v1.

## Components

### `airplay-source.*`

Registers the new OBS source and owns OBS-facing source lifecycle:

- Create and destroy source context.
- Read settings.
- Start, stop, and restart `AirPlayRuntime`.
- Report width and height from the runtime.
- Build source properties and defaults.

### `airplay-runtime.*`

Owns the AirPlay receiver:

- Starts `fgServerStart(receiver_name, raop_port, airplay_port, callback)`.
- Stops the server on source destruction or restart.
- Implements `IAirServerCallback`.
- Accepts one active Apple sender at a time by remote device ID.
- Converts decoded video/audio callback data to shared OBS output helpers.
- Bridges AirPlay logs to OBS logs with an `[obs-airplay-source]` prefix.

### AirPlayServer Integration

Use the AirPlayServer callback surface:

- `SFgVideoFrame` for decoded YUV420 video frames.
- `SFgAudioFrame` for decoded PCM audio frames.
- `connected()` and `disconnected()` for active-device state.
- `outputVideo()` and `outputAudio()` for media delivery.
- `videoPlay()` and related media playback callbacks as no-op unsupported
  callbacks.

The integration should avoid SDL and ImGui. Only the receiver/core/wrapper
pieces needed to expose the callback API should be linked or vendored.

## Data Flow

```text
Apple device
  -> AirPlay/RAOP over LAN
  -> Bonjour-advertised embedded AirPlay receiver
  -> AirPlayServer decoded video/audio callbacks
  -> AirPlayRuntime active-device filter
  -> shared OBS media output helpers
  -> obs_source_output_video()
  -> obs_source_output_audio()
  -> OBS scene/output encoder
```

## OBS Source Settings

The first version should keep settings focused:

- Receiver name: default `OBS AirPlay`.
- RAOP/audio port: default `5001`.
- AirPlay/mirroring port: default `7001`.
- Restart receiver button.
- Help/status text for Bonjour, firewall, and same-subnet requirements.

Port fields should validate to the user-port range `1..65535`. Invalid or
empty receiver names should fall back to `OBS AirPlay`.

## Runtime Behavior

- Each `Apple AirPlay Source` starts its own AirPlay receiver.
- One active Apple device is accepted per source. If another device connects
  while one is active, its callbacks are ignored until the current device
  disconnects.
- On disconnect, the source clears video output and waits for the next sender.
- If startup fails because Bonjour/DNS-SD is unavailable, the source logs a
  clear warning and leaves the source idle.
- If startup fails because a port is already in use, the source logs a clear
  warning and waits for a settings change or manual restart.
- Media playback callbacks are no-op and log once that media casting is not
  supported by this source.

## Packaging

The installed global plugin location remains the authoritative runtime target:

```text
C:\Program Files\obs-studio\obs-plugins\64bit
```

The installer should copy only the DLLs needed by the embedded AirPlay receiver
and the plugin. It should not install or launch `AirPlayServer.exe`.

Before distributing builds, document attribution and license obligations for:

- `xenos1337/AirPlayServer` itself.
- AirPlayServer's embedded AirPlay/RAOP, plist, crypto, AAC, FFmpeg, and
  DNS-SD related dependencies.
- Bonjour for Windows as an external prerequisite for device discovery.

## Error Handling

- Bonjour missing or stopped: OBS warning plus source-property help text.
- Firewall or network isolation: OBS warning/help text explaining that the
  Apple device and PC must be on the same subnet and that Windows Firewall must
  allow OBS on the private network.
- Port collision: OBS warning naming the configured ports.
- Callback after shutdown: ignored safely by checking runtime stop state before
  output.
- Unsupported media-cast request: no-op, logged once per runtime start.

## Testing

### Code-Level Tests

- AirPlay settings defaults and validation.
- `SFgVideoFrame` to OBS I420 plane mapping, including Y, U, V offsets and
  pitch.
- `SFgAudioFrame` to OBS audio metadata, including sample rate, channels,
  bits per sample, frame count, and timestamp handling.
- Existing scrcpy command and low-latency tests remain passing.

### Build Verification

- Build the plugin with the embedded AirPlay receiver linked.
- Confirm the plugin DLL loads in OBS.
- Confirm both `scrcpy Device Source` and `Apple AirPlay Source` appear.
- Confirm the global install script deploys the new plugin and required
  runtime DLLs to the OBS install directories.

### Device Verification

- Add `Apple AirPlay Source` in OBS.
- Confirm the receiver appears in iPhone/iPad/macOS screen mirroring.
- Start mirroring and verify video and device audio arrive in OBS.
- Confirm no external AirPlayServer window appears.
- Disconnect and reconnect the Apple device.
- Confirm source dimensions update when orientation or sender resolution
  changes.
- Inspect OBS logs for Bonjour, port, connection, callback, disconnect, and
  unsupported media-cast messages.

## Open Implementation Notes

- The source tree has significant existing uncommitted scrcpy work. AirPlay
  implementation should be staged carefully and should not revert unrelated
  changes.
- The installed plugin DLL at
  `C:\Program Files\obs-studio\obs-plugins\64bit\obs-scrcpy-source.dll`
  is the current deployed backbone and reports version `0.2.0`.
- The AirPlay source should preserve the existing Android source behavior while
  adding the new OBS source registration.
