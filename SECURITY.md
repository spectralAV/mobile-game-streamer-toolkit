# Security Policy

Mobile Game Streamer Toolkit runs locally inside OBS Studio and includes an optional elevated Windows helper (`obs-airplay-tether-helper.exe`) for iPhone USB Reverse Tethering and local loopback frame bridging. Security reports are welcome around named-pipe IPC, elevated helper execution, installer PowerShell scripts, network receiver sockets, and dependency safety.

## Supported Versions

| Version | Security support |
| --- | --- |
| `0.3.x` | Active public development support |
| Earlier versions | Not supported — upgrade to `v0.3.7` or newer |

## Reporting A Vulnerability

Use GitHub private vulnerability reporting on `spectralAV/mobile-game-streamer-toolkit` if enabled. If private reporting is unavailable, open a minimal public issue asking for a security contact without posting exploit details.

Please include:

- Affected version or commit SHA.
- Windows version/build and OBS Studio version.
- Reproduction steps and affected component (`obs-scrcpy-source.dll`, `obs-airplay-tether-helper.exe`, or installer scripts).
- Security impact (privilege escalation, arbitrary file write, remote memory corruption, etc.).

Do not post private pairing keys, device serial numbers, auth tokens, private gameplay captures, or exploit payloads in public issues.

## Scope

In scope:

- Named-pipe IPC (`\\.\pipe\obs-airplay-tether`) and elevated helper command validation.
- Unsafe file writes, path traversal, or unquoted path execution in installer/helper scripts.
- Memory safety in `scrcpy` packet framing, Annex B H.264 parsing, and audio/video buffer handling.
- Local HTTP frame bridge (`127.0.0.1:8787`) safety.

Out of scope:

- Issues requiring physical access to an already-unlocked administrator session.
- Upstream vulnerabilities in unpatched OBS Studio, Android `adb`, Apple Bonjour, or Windows ICS itself.
- Requests to bypass iOS "Trust This Computer" prompts, Android USB debugging authorization, or DRM protections.
