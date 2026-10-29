# Known Limitations

## Platform Support

- **Windows 10 / 11 (x64)** is the primary supported and packaged platform (`OBS Studio 30+ / 32+` 64-bit).
- macOS and Linux CMake scaffolding from `obs-plugintemplate` is present in `cmake/`, but AirPlay runtime loading and iPhone USB Reverse Tethering use Windows-specific APIs (`D3D11VA`, `NetSharingManager` ICS, `CfgMgr32`, `libusb0`).

## Android `scrcpy` Source

- Capture is intentionally **read-only** (no mouse/keyboard control injection inside OBS).
- DRM-protected apps (`FLAG_SECURE` / Widevine L1 video surfaces) render black frames per Android OS security policy.
- Hardware encoder availability varies by SoC (`c2.qti.*`, `c2.google.*`, `c2.mtk.*`, `OMX.*`); leave **Video encoder** on **Auto** unless tuning for a specific device.

## Apple AirPlay & iPhone USB Reverse Tethering

- Supports **Screen Mirroring** and **device gameplay/system audio**. DRM-protected video URL casting (FairPlay streaming apps) is not supported.
- **iPhone USB Reverse Tethering** requires:
  - A data-capable USB cable (not a charge-only cable).
  - Unlocking the iPhone screen and tapping **Trust** on first connection.
  - An active PC internet uplink (Ethernet or Wi-Fi) so Windows Internet Connection Sharing (`192.168.137.1`) can assign a DHCP lease to the iPhone's CDC-NCM USB Ethernet interface.
- Windows ICS shares a single active uplink at a time; enabling USB Reverse Tethering reconfigures ICS for the duration of the tethering session and disables ICS cleanly when tethering stops or OBS exits.
