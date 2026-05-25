# Architecture

## Why a native source

The requested behavior cannot be cleanly delivered by window capture, because the scrcpy client window would still exist and OBS would be capturing a rendered desktop window. The source should instead connect to the scrcpy transport directly and publish decoded frames/audio to OBS.

## Data path

```text
Android device
  -> scrcpy server
  -> ADB tunnel sockets
  -> OBS plugin transport reader
  -> FFmpeg decoders
  -> obs_source_output_video()
  -> obs_source_output_audio()
  -> OBS scene/output encoder
  -> optional throttled localhost BMP bridge
  -> MLBB CoPilot CV/reasoning pipeline
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
- `--audio-buffer=<ms>`
- `--audio-codec-options=<MediaFormat options>`

The OBS source should expose bit rate and buffer as first-class fields, with codec options as an advanced field.

## Implementation stages

1. OBS source registration and settings UI. Done.
2. Local capability probing with `scrcpy --list-encoders`. Done.
3. ADB tunnel/session startup matching scrcpy 4.0. Done for USB/selected serial via `adb reverse`.
4. Video packet reader and codec header handling. Done.
5. FFmpeg decode to OBS async video frames. Done for decoded `yuv420p` frames.
6. Audio packet reader and FFmpeg decode/resampling to OBS audio frames. Done to 48 kHz stereo S16.
7. Reconnect handling and device switching. Basic restart on settings update; richer auto-reconnect remains future work.
8. Installer/package layout for OBS Studio on Windows. Done for per-user OBS plugin install.
9. Optional local MLBB CoPilot frame bridge. Done as a throttled decoded-frame HTTP publisher; crop-specific transport and shared memory remain future optimizations.
