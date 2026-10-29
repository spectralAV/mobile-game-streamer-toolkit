# Architecture

## Why a native source

The requested behavior cannot be cleanly delivered by window capture, because the scrcpy client window would still exist and OBS would be capturing a rendered desktop window. The source should instead connect to the scrcpy transport directly and publish decoded frames/audio to OBS.

## Data path

```text
Android device
  -> scrcpy server
  -> ADB tunnel sockets
  -> bounded OBS plugin transport reader
  -> FFmpeg D3D11VA decoder or threaded software fallback
  -> obs_source_output_video() in async-unbuffered mode
  -> obs_source_output_audio()
  -> OBS scene/output encoder
  -> optional throttled localhost raw BGRA bridge
  -> MLBB CoPilot CV/reasoning pipeline
```

Apple devices use a separate OBS source and transport path:

```text
iPhone/iPad/Mac
  -> AirPlay/RAOP over LAN
  -> embedded AirPlayServer receiver from airplay2dll.dll
  -> decoded YUV420 video and PCM audio callbacks
  -> obs_source_output_video() in async-unbuffered mode
  -> obs_source_output_audio()
  -> OBS scene/output encoder
```

## Read-only mode

The plugin must always configure the scrcpy server with control disabled. In command-line terms, this is equivalent to:

```text
--no-control
```

The source settings should not expose any control toggle.

## Video codec choices

scrcpy 4.1 supports:

- `h264`
- `h265`
- `av1`
- `vp8`
- `vp9`

The codec selected here affects the device-side MediaCodec encoder and the USB/TCP transport stream. OBS may still encode the final stream or recording using a different codec in OBS output settings.

## Audio quality controls

scrcpy 4.0/4.1 supports:

- `--audio-codec=opus|aac|flac|raw`
- `--audio-bit-rate=<value>`
- `--audio-codec-options=<MediaFormat options>`

The OBS source exposes bit rate and audio delay as first-class fields, with codec
options as an advanced field. The native server protocol has no `audio_buffer`
option, so the delay is applied to OBS audio timestamps locally.

## Implementation stages

1. OBS source registration and settings UI. Done.
2. Local capability probing with `scrcpy --list-encoders`. Done.
3. ADB tunnel/session startup matching scrcpy 4.0 and 4.1. Done for USB/selected serial via `adb reverse`; the selected package version is detected and passed to the exact-version server handshake.
4. Video packet reader and codec header handling. Done.
5. FFmpeg decode to OBS async video frames. Done with D3D11VA plus threaded software fallback and latest-frame OBS presentation. During measured lag, dependency frames continue decoding while stale presentation is suppressed; multi-second clock discontinuities reconnect at the live edge instead of replaying a stale queue. AV1 prefers dav1d or FFmpeg's native decoder over libaom.
6. Audio packet reader and FFmpeg decode/resampling to OBS audio frames. Done to 48 kHz stereo S16 on the same device PTS timeline as decoded video, with stale-audio dropping and configurable timestamp delay. OBS therefore applies one timing adjustment to both streams instead of reconciling independent wall-clock and media-clock timestamps.
7. Reconnect handling and device switching. Done: the runtime verifies the selected device and reconnects dropped sessions with exponential backoff capped at 60 seconds, without restarting the global ADB daemon. A three-second first-frame watchdog also reconnects sessions whose sockets open but whose decoder never produces startup video.
8. Installer/package layout for OBS Studio on Windows. Done for the global OBS plugin install with the full shared FFmpeg runtime required by D3D11VA.
9. Optional local MLBB CoPilot frame bridge. Done as a throttled decoded-frame HTTP publisher; crop-specific transport and shared memory remain future optimizations.

## Apple AirPlay source

The plugin also registers `Apple AirPlay Source` with source id
`obs_airplay_source`. It dynamically loads `airplay2dll.dll` (AirPlayServer
v1.2.5 pure native MSVC build), starts headless streaming via
`fgServerStartHeadless()` (or fallback `fgServerStart()`), and implements the
13-method `IAirServerCallback` interface.

Video delivery uses a dual-path architecture:
1. **Headless H.264 Access Units**: `outputH264AccessUnit()` delivers raw NAL units.
   Annex B SPS (type 7) and PPS (type 8) parameter sets are cached and prepended to
   incoming keyframes (IDR slices) to prevent decoder state resets and `AVERROR_INVALIDDATA`
   rejections. Frames are decoded in OBS via low-latency `libavcodec` software decoding
   and published to `obs_source_output_video()`.
2. **Decoded Frame Fallback**: `outputVideo()` packs strided I420 planes into a
   contiguous memory buffer before invoking `obs_source_output_video()`, eliminating
   access violation crashes in `obs.dll!copy_frame_data_plane`.

The runtime uses Windows BCrypt and native MSVC threading, completely eliminating
legacy `msys-2.0.dll` TLS conflict crashes. The DLL handle remains resident across
source restarts to prevent worker thread teardown races. Enabled sources start
automatically on OBS scene load without requiring deferred startup workarounds.

This source is screen/audio capture only. AirPlay media URL playback callbacks
are no-ops and are logged as unsupported.
