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

Mobile operating systems block external screen capture by default for security. Before OBS Studio can capture gameplay video and audio, complete the steps below for your device.

### Android Setup (`scrcpy Device Source` — USB Debugging Required)

`scrcpy` connects to Android over **ADB (Android Debug Bridge)**, which requires unlocking the hidden **Developer options** menu and enabling **USB debugging** once on your phone:

1. **Unlock hidden Developer options**:
   Open **Settings** on your Android device, locate the build/OS version entry for your phone brand below, **tap it 7 times quickly**, and enter your lock-screen PIN/pattern when prompted until you see *"You are now a developer!"*:

   | Phone Brand | Where to tap 7 times | Where Developer options appears | Brand-specific note |
   | --- | --- | --- | --- |
   | **Google Pixel / Stock Android** | `Settings -> About phone -> Build number` | `Settings -> System -> Developer options` | — |
   | **Samsung Galaxy (One UI)** | `Settings -> About phone -> Software information -> Build number` | `Settings -> Developer options` (very bottom of Settings) | On **One UI 6+**, if **USB debugging** is grayed out, go to `Settings -> Security and privacy -> Auto Blocker` and turn **Auto Blocker OFF** |
   | **Xiaomi / POCO / Redmi (HyperOS / MIUI)** | `Settings -> About phone -> OS version` *(or `MIUI version`)* | `Settings -> Additional settings -> Developer options` | Only standard **USB debugging** is required (read-only capture does **not** need *USB debugging (Security settings)*) |
   | **OnePlus / OPPO / Realme / ASUS ROG** | `Settings -> About device -> Version -> Version number` *(ROG: `Software information -> Build number`)* | `Settings -> Additional settings` *(or `System`)* `-> Developer options` | Turn on **Disable adb authorization timeout** if available so permission stays permanent |

2. **Turn on USB debugging**:
   Open **Developer options**, scroll down to **Debugging**, toggle **USB debugging** **ON**, and tap **OK** on the confirmation prompt.
3. **Connect with a USB data cable & allow your PC**:
   - Plug your phone into your PC using a **USB data cable** *(note: many power-bank or accessory cables are charge-only and have no data wires)*.
   - Keep your phone screen **unlocked** and watch for the **"Allow USB debugging?"** popup on the phone.
   - Check **"Always allow from this computer"** and tap **Allow**.
   - *(Tip: If no popup appears, swipe down your Android notification shade, tap the **Charging this device via USB** notification, and switch USB mode to **File Transfer / Android Auto** or **PTP**.)*
4. **Select your phone in OBS Studio**:
   - In OBS Studio, add **scrcpy Device Source**, click **Refresh devices**, and choose your phone under **Device serial** (or keep `Auto USB device`).
   - *Optional Wireless (`TCP/IP`) mode*: After authorizing over USB once, run `"C:\Program Files\scrcpy\4.1\adb.exe" tcpip 5555` (or enable **Wireless debugging**), set **Connection** to `TCP/IP`, and enter `<phone-ip>:5555`.

---

### Apple iPhone / iPad / Mac Setup (`Apple AirPlay Source` — Control Center Screen Mirroring)

Unlike capture cards or Android `scrcpy`, Apple devices **never** start streaming automatically just from plugging in a cable or adding the OBS source. You always start the stream from your Apple device's **Control Center (quick settings bar)** using **Screen Mirroring**:

#### Option A — Wireless AirPlay (5 GHz Wi-Fi / LAN)

1. Connect your iPhone/iPad/Mac and your Windows PC to the **same local network** (5 GHz Wi-Fi or wired Ethernet on the same router/subnet; avoid isolated "Guest" Wi-Fi networks).
2. In OBS Studio, add **Apple AirPlay Source**, check **Enable AirPlay receiver**, and click **OK** (default receiver name: **`OBS AirPlay`**).
3. Open **Control Center (quick settings bar)** on your Apple device:
   - **iPhone (Face ID) & modern iPad**: **Swipe down from the top-right corner** of the screen (where the battery and Wi-Fi icons sit).
   - **iPhone SE / Home-button models**: **Swipe up from the bottom edge** of the screen.
   - **Mac (macOS)**: Click the **Control Center** icon (two pill toggles) in the top-right menu bar.
4. Tap the **Screen Mirroring** button *(icon with two overlapping rectangles)* and select **`OBS AirPlay`**. Gameplay video and audio will stream directly into OBS.

#### Option B — Zero-Lag USB Cable (iPhone USB Reverse Tethering)

1. Plug your iPhone into your PC with a Lightning or USB-C data cable, **unlock the iPhone**, and tap **Trust** (plus enter your iPhone passcode) if **"Trust This Computer?"** appears.
2. In OBS Studio, open **Apple AirPlay Source** properties and check **both**:
   - **Enable AirPlay receiver**
   - **Enable iPhone USB Reverse Tethering (Zero-Lag / Direct Cable)**
3. Wait 3–5 seconds (click **Refresh USB Status** to confirm status is **`Active`** — your iPhone now uses a private direct USB Ethernet link to your PC).
4. **Start Screen Mirroring on your iPhone**: Even over USB cable, **swipe down from the top-right corner** of your iPhone to open **Control Center (quick settings bar)** -> tap **Screen Mirroring** (two overlapping rectangles) -> select **`OBS AirPlay`**.
5. **When finished**: Open **Control Center -> Screen Mirroring -> Stop Mirroring**. Whenever you uncheck **Enable iPhone USB Reverse Tethering** or close OBS Studio, your iPhone automatically reverts to **100% stock Apple USB drivers** (Windows Photos, File Explorer, iTunes, and Apple Devices work normally).

---

## 5. Common First-Time Device Setup Issues

| Symptom | Likely Cause | Fix |
| --- | --- | --- |
| Android phone does not appear when clicking **Refresh devices** | **USB debugging** is off, phone is using a **charge-only USB cable**, or **"Allow USB debugging?"** was not accepted | Unlock **Developer options -> USB debugging**, swap to a known USB data cable, unlock the phone screen, switch USB mode to **File Transfer**, and tap **Allow** on the prompt |
| **USB debugging** toggle is grayed out on Samsung Galaxy | Samsung **Auto Blocker** (enabled by default on One UI 6+) blocks ADB | Go to `Settings -> Security and privacy -> Auto Blocker` and turn it **OFF** |
| Android phone appears once, then stays stuck on `"Device disconnected"` | RSA authorization prompt expired or was dismissed without tapping **Allow** | In Android **Developer options**, tap **Revoke USB debugging authorizations**, unplug and reconnect USB, check **Always allow from this computer**, and tap **Allow** |
| **`OBS AirPlay`** does not show up in iPhone/iPad **Screen Mirroring** | **Enable AirPlay receiver** is unchecked in OBS, devices are on different Wi-Fi networks / Guest Wi-Fi isolation, or Bonjour is stopped | Check **Enable AirPlay receiver** in OBS properties, verify PC & iPhone share the same private LAN / 5 GHz Wi-Fi (or enable **USB Reverse Tethering**), and ensure `Bonjour Service` is running |
| Enabled **iPhone USB Reverse Tethering** (`Active`), but OBS still shows placeholder | USB Reverse Tethering creates the cable network tunnel, but iOS still waits for you to start mirroring | Unlock iPhone -> **swipe down from top-right corner** (**Control Center**) -> tap **Screen Mirroring** -> select **`OBS AirPlay`** |


