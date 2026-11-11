# DualSense BB Controller Audio & Haptics Fix for Death Stranding: Director's Cut

[![Platform](https://img.shields.io/badge/Platform-Windows%20x64-blue.svg)](https://github.com/Gythiro/DualSense_BB_Audio_Fix)
[![Game](https://img.shields.io/badge/Game-Death%20Stranding%20Director's%20Cut-green.svg)](https://store.steampowered.com/app/1850570)
[![License](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Nexus Mods](https://img.shields.io/badge/Nexus%20Mods-Mod%20133-orange.svg)](https://www.nexusmods.com/deathstranding/mods/133)

**[简体中文文档](README_ZH.md)**

---

An open-source, clean-room reimplementation and extension of the archived Nexus Mods #110 (`XAudio2_9.dll` fix by Kravitz) for **DEATH STRANDING DIRECTOR'S CUT (PC / Steam / Epic Games)**.

It fixes three fundamental audio architecture bugs in the PC port (`ds.exe`), allowing **DualSense controller speaker audio (BB cries/voice)**, **native 4-channel 48kHz HD haptic feedback**, and **adaptive triggers** to function simultaneously without conflicts or audio device workarounds.

---

## Key Features

- **Concurrent HD Haptics & Speaker Voice**: Unlocks the WASAPI Exclusive lock by bridging the game's haptics audio stream to Shared Mode while preserving native 4-channel 48kHz HD vibration.
- **In-Game Output Selector Bugfix**: Fixes the menu registration bug where toggling the BB audio device was delayed or inverted. Selecting the DualSense speaker in the settings menu now takes effect immediately on the first attempt.
- **DualSense HID Speaker Routing (Haute 4DS / Wired USB)**: Patches outgoing DualSense HID Output Reports (`0x02`) so the internal speaker is properly enabled when using wired USB or wireless adapters (such as Haute 4DS in "Follow game" mode). Respects physical 3.5mm headset connection.
- **Zero Dependencies**: Pure C++17 proxy DLL with zero external runtime requirements. Plug and play.

---

## Root Cause & Technical Details

1. **Bug 1 (Core Conflict): WASAPI Exclusive Lock in `PadAudioPlayback` (`RVA 0x19B6610`)**
   - The game's native haptics thread (`PadAudioPlayback`) initializes the 4-channel DualSense audio endpoint using `AUDCLNT_SHAREMODE_EXCLUSIVE`. It sends vibration waveforms to rear channels (`ch2/ch3`), while zeroing out front channels (`ch0/ch1`).
   - Because the endpoint is locked exclusively, the Wwise sound engine fails to open `DS_Sub_Output` (Shared Mode, BB audio) on the front channels, returning `AUDCLNT_E_DEVICE_IN_USE (0x8889000A)` and falling back to main speakers or silence. Disabling exclusive mode in Windows causes `PadAudioPlayback` initialization to crash/abort, killing HD haptics entirely.
   - **Fix**: Hooks `ole32.dll!CoCreateInstance` (IAT & Delay-IAT) to wrap `PadAudioPlayback`'s `IAudioClient` into a low-latency WASAPI Shared Mode stream (`16-bit PCM -> 32-bit Float`, 480 frames / 10ms period). The Windows Audio Engine seamlessly mixes BB voice (`ch0/ch1`) and HD haptics (`ch2/ch3`) simultaneously.

2. **Bug 2: Output Device Swap Register Omission (`RVA 0x1DD4A00`)**
   - In `UpdateSubOutput`, the game branches into `AK::SoundEngine::ReplaceOutput`. The developer omitted `mov r8d, ebx`, accidentally passing the previous device ID (`r8d`) instead of the newly selected device (`ebx`).
   - This caused the notorious "menu inversion" issue where selecting the controller routed audio to PC speakers, requiring a double-switch.
   - **Fix**: Pattern-scans and patches the missing `mov r8d, ebx` instruction in memory, ensuring immediate and accurate device switching.

3. **Bug 3: DualSense USB HID Speaker Routing & Volume Disable**
   - In `libScePad`, outgoing USB Output Report `0x02` sets `AudioControl` to `0x0C` when no headset is detected, leaving `OutputPathSelect` at `0` (`L-R X`, headphone-only, speaker unrouted). It also omits the `AllowSpeakerVolume` flag and hardware gain.
   - On direct USB connections or wireless adapters set to "Follow game", the controller hardware mutes its internal speaker.
   - **Fix**: Hooks `KERNEL32!WriteFile` to identify DualSense output packets. When no 3.5mm headset is connected, it updates `OutputPathSelect` to `3` (`X-X R`), sets hardware speaker volume to `0x64` (100%), and applies pre-amp gain (`+2`).

---

## Installation

### Method 1: Manual Copy (Recommended & Easiest)
1. Download the release archive (`DualSense_BB_Audio_Fix_vX.X.X.zip`) from [Releases](https://github.com/Gythiro/DualSense_BB_Audio_Fix/releases) or [Nexus Mods](https://www.nexusmods.com/deathstranding/mods/133).
2. Extract the archive.
3. Copy **`XAudio2_9.dll`** and **`XAudio2_9_BBFix.ini`** directly into your Death Stranding Director's Cut installation folder (the folder containing `ds.exe`):
   - Example Steam path: `C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING DIRECTORS CUT\`

### Method 2: One-Click Installer Script (`install.bat`)
1. Extract the release archive.
2. Double-click `install.bat`. It will automatically scan standard Steam and Epic Games installation directories.
3. **Custom Installation Path**: If your game is installed in a non-standard drive or folder:
   - Right-click `install.bat` and select **Edit** (with Notepad).
   - Set your directory on the `set "CUSTOM_GAME_DIR="` line (e.g., `set "CUSTOM_GAME_DIR=D:\Games\DEATH STRANDING DIRECTORS CUT"`).
   - Alternatively, simply run `install.bat` and drag-and-drop your game folder into the command window when prompted.

---

## Setup Checklist

### 1. Steam Settings
- Right-click **DEATH STRANDING DIRECTOR'S CUT** in your Steam Library -> **Properties** -> **Controller**.
- Set **Override for DEATH STRANDING DIRECTOR'S CUT** to **Disable Steam Input**.
  *(Steam Input acts as a generic gamepad layer and blocks native DualSense 4-channel audio & HD haptics).*

### 2. Windows Sound Settings (`mmsys.cpl`)
- Keep your primary speakers / headset as the Windows **Default Device**.
- Verify that `Wireless Controller` (DualSense) is enabled and set to 100% volume in the Playback tab.

### 3. Controller / Adapter Modes
- **USB Cable**: Plug and play.
- **Haute 4DS / DS5Dongle Adapter**:
  - In the web configuration interface ([engine.haute42.com](https://engine.haute42.com)), **DS Speaker Mode** can be set to either **Follow game** or **Always on**.
  - Set **Speaker Gain** to `2` (recommended).
  - Set **Audio Haptics** to `Off` (to preserve native in-game HD haptics).

### 4. In-Game Settings
1. Open **Options** -> **Controller Settings**: Ensure **Controller Speaker** = `On`, **Vibration** = `On`.
2. Open **Options** -> **Audio Settings**: Select **`Wireless Controller`** as the **BB Voice Output Device**. It will take effect immediately.

---

## Configuration Reference (`XAudio2_9_BBFix.ini`)

| Option | Default | Description |
|---|---|---|
| `EnableSharedModeHaptics` | `1` | Bridges exclusive haptics to shared mode (core fix). |
| `FixBBDeviceSwapBug` | `1` | Fixes the menu device selector inversion bug. |
| `FixDualSenseSpeakerRouting`| `1` | Hooks HID Output reports to route audio to internal speaker. |
| `SpeakerOutputPath` | `3` | `3` = Right channel to speaker (recommended); `2` = Left to headphone, Right to speaker. |
| `SpeakerVolume` | `100` | Controller speaker hardware volume level (range: 61 - 100). |
| `SpeakerPreampGain` | `2` | Speaker pre-amplifier gain level (range: 0 - 7). |
| `HapticsGain` | `1.0` | Multiplier for HD haptics vibration intensity (0.0 to 4.0). |
| `EnableLog` | `1` | Generates diagnostic log `XAudio2_9_BBFix.log` in the game folder. |

---

## Building from Source

Requirements: [Zig Compiler](https://ziglang.org/download/) (0.13.0 or later).

```cmd
git clone https://github.com/Gythiro/DualSense_BB_Audio_Fix.git
cd DualSense_BB_Audio_Fix
build.bat
```
The script compiles `src/xaudio2_proxy.cpp` and `src/XAudio2_9.def` into a standalone `XAudio2_9.dll`.

---

## Uninstallation

- Delete `XAudio2_9.dll`, `XAudio2_9_BBFix.ini`, and `XAudio2_9_BBFix.log` from your game folder.
- Or double-click `uninstall.bat` to restore vanilla state automatically.

---

## Credits & Acknowledgements

- **Kravitz**: For the original discovery of the WASAPI exclusive lock and menu device swap bug in Nexus Mods #110.
- **Sony Linux Kernel Driver**: Reference for DualSense Output Report `0x02` bitfields and audio routing protocols (`drivers/hid/hid-playstation.c`).

---

## License

This project is licensed under the [MIT License](LICENSE).
