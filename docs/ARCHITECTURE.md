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

scrcpy 4.0 supports:

- `h264`
- `h265`
- `av1`

The codec selected here affects the device-side MediaCodec encoder and the USB/TCP transport stream. OBS may still encode the final stream or recording using a different codec in OBS output settings.

## Audio quality controls

scrcpy 4.0 supports:

- `--audio-codec=opus|aac|flac|raw`
- `--audio-bit-rate=<value>`
- `--audio-codec-options=<MediaFormat options>`

The OBS source exposes bit rate and audio delay as first-class fields, with codec
options as an advanced field. The native server protocol has no `audio_buffer`
option, so the delay is applied to OBS audio timestamps locally.

## Implementation stages

1. OBS source registration and settings UI. Done.
2. Local capability probing with `scrcpy --list-encoders`. Done.
3. ADB tunnel/session startup matching scrcpy 4.0. Done for USB/selected serial via `adb reverse`.
4. Video packet reader and codec header handling. Done.
5. FFmpeg decode to OBS async video frames. Done with D3D11VA plus threaded software fallback and latest-frame OBS presentation. During measured lag, dependency frames continue decoding while stale presentation is suppressed; once the socket backlog drains, presentation resumes and the host clock is rebased.
6. Audio packet reader and FFmpeg decode/resampling to OBS audio frames. Done to 48 kHz stereo S16, with configurable timestamp delay applied locally.
7. Reconnect handling and device switching. Done: the runtime verifies the selected device and reconnects dropped sessions with exponential backoff capped at 60 seconds, without restarting the global ADB daemon.
8. Installer/package layout for OBS Studio on Windows. Done for the global OBS plugin install with the full shared FFmpeg runtime required by D3D11VA.
9. Optional local MLBB CoPilot frame bridge. Done as a throttled decoded-frame HTTP publisher; crop-specific transport and shared memory remain future optimizations.

## Apple AirPlay source

The plugin also registers `Apple AirPlay Source` with source id
`obs_airplay_source`. It dynamically loads `airplay2dll.dll`, starts
`fgServerStart()` with the configured receiver name and ports, implements the
AirPlayServer callback interface, filters callbacks to one active Apple device,
and maps `SFgVideoFrame`/`SFgAudioFrame` payloads directly to OBS async video
and audio frames.

Receiver startup is deliberately deferred during scene restoration. The bundled
third-party AirPlay runtime has previously faulted in its video worker, so only
an explicit **Restart receiver** action may enter `fgServerStart()`; this keeps a
saved enabled source from creating an OBS startup crash loop.

This source is screen/audio capture only. AirPlay media URL playback callbacks
are no-ops and are logged as unsupported.
