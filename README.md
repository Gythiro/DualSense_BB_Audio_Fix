# DualSense BB Controller Audio & Haptics Fix for Death Stranding: Director's Cut

[![Platform](https://img.shields.io/badge/Platform-Windows%20x64-blue.svg)](https://github.com/Gythiro/DualSense_BB_Audio_Fix)
[![Game](https://img.shields.io/badge/Game-Death%20Stranding%20Director's%20Cut-green.svg)](https://store.steampowered.com/app/1850570)
[![License](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

[English](#english) | [中文说明](#中文说明)

---

<a name="english"></a>
## English

An open-source, clean-room reimplementation and extension of the archived Nexus Mods #110 (`XAudio2_9.dll` fix by Kravitz) for **Death Stranding Director's Cut (PC / Steam / Epic)**.

It fixes three fundamental audio architecture bugs in `ds.exe`, allowing **DualSense controller speaker audio (BB cries/voice)**, **native HD haptic feedback**, and **adaptive triggers** to function simultaneously without conflicts or audio device workarounds.

### Key Features
- **Concurrent HD Haptics & Speaker Voice**: Unlocks the WASAPI Exclusive lock by bridging the game's haptics audio stream to Shared Mode while preserving native 4-channel 48kHz HD vibration.
- **In-Game Output Selector Bugfix**: Fixes the menu registration bug where toggling the BB audio device was delayed or inverted. Selecting the DualSense speaker in the settings menu now takes effect immediately on the first attempt.
- **DualSense HID Speaker Routing (Haute 4DS / Wired USB)**: Patches outgoing DualSense HID Output Reports (`0x02`) so the internal speaker is properly enabled when using wired USB or wireless adapters (such as Haute 4DS in "Follow game" mode). Respects 3.5mm headset detection.
- **Zero Dependencies**: Pure C++17 proxy DLL with zero external runtime requirements. Drops directly into the game folder.

---

### Root Cause & Technical Details

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

### Quick Installation

1. Download the latest release package (`DualSense_BB_Audio_Fix_vX.X.zip`) from [Releases](https://github.com/Gythiro/DualSense_BB_Audio_Fix/releases).
2. Extract the archive and copy both `XAudio2_9.dll` and `XAudio2_9_BBFix.ini` directly into your game folder (where `ds.exe` is located):
   - Example Steam path: `steamapps\common\DEATH STRANDING DIRECTORS CUT\`
3. (Optional) You can also run `安装到死亡搁浅.bat` for one-click installation.

---

### Setup Checklist

#### 1. Steam Settings
- Right-click **Death Stranding: Director's Cut** in Steam -> **Properties** -> **Controller**.
- Set **Override for Death Stranding Director's Cut** to **Disable Steam Input**.
  *(Steam Input interferes with DualSense native haptics and multi-channel audio).*

#### 2. Windows Sound Settings (`mmsys.cpl`)
- Ensure your primary audio device (DAC/Headphones/Speakers) is set as the **Default Device**.
- Verify that `Wireless Controller` (DualSense) is enabled and set to 100% volume.
- Default Windows settings ("Allow applications to take exclusive control") can remain enabled.

#### 3. Controller / Adapter Modes
- **USB Cable**: Plug and play.
- **Haute 4DS / DS5Dongle**:
  - In the web configuration interface ([engine.haute42.com](https://engine.haute42.com)), **DS Speaker Mode** can be set to either **Follow game** or **Always on**.
  - Set **Speaker Gain** to `2` (recommended).
  - Set **Audio Haptics** to `Off` (to preserve native in-game HD haptics).

#### 4. In-Game Settings
1. Go to **Options** -> **Controller Settings**: Set **Controller Speaker** = `On`, **Vibration** = `On`.
2. Go to **Options** -> **Audio Settings**: Select **`Wireless Controller`** as the **BB Voice Output Device**. It will take effect immediately.

---

### Configuration Reference (`XAudio2_9_BBFix.ini`)

| Option | Default | Description |
|---|---|---|
| `EnableSharedModeHaptics` | `1` | Bridges exclusive haptics to shared mode (core fix). |
| `FixBBDeviceSwapBug` | `1` | Fixes the menu device selector inversion bug. |
| `FixDualSenseSpeakerRouting`| `1` | Hooks HID Output reports to route audio to internal speaker. |
| `SpeakerOutputPath` | `3` | `3` = Right channel to speaker; `2` = Left to headphone, Right to speaker. |
| `SpeakerVolume` | `100` | Controller speaker hardware volume (range: 61 - 100). |
| `SpeakerPreampGain` | `2` | Speaker pre-amplifier gain level (range: 0 - 7). |
| `HapticsGain` | `1.0` | Multiplier for HD haptics vibration intensity (0.0 to 4.0). |
| `EnableLog` | `1` | Generates diagnostic log `XAudio2_9_BBFix.log` in the game folder. |

---

### Building from Source

Requirements: [Zig Compiler](https://ziglang.org/download/) (0.13.0 or later).

```cmd
git clone https://github.com/Gythiro/DualSense_BB_Audio_Fix.git
cd DualSense_BB_Audio_Fix
build.bat
```
The script compiles `src/xaudio2_proxy.cpp` and `src/XAudio2_9.def` into a lightweight, standalone `XAudio2_9.dll`.

---
---

<a name="中文说明"></a>
## 中文说明

针对 PC 版《死亡搁浅：导演剪辑版》（`ds.exe`）从零编写的开源纯净修复补丁（基于原 Nexus Mods #110 作者 Kravitz 的逆向分析进行重制与深度扩展）。

彻底解决官方游戏在 PC 平台上的三大底层音频架构缺陷，实现 **手柄喇叭独立播放 BB 语音**、**原生 HD 触觉震动反馈** 与 **自适应扳机** 完美共存，告别各种繁琐的音频配置偏方。

### 核心特性
- **HD 触觉与手柄语音完美共存**：无感解除 WASAPI 独占设备锁，将原生触觉流桥接为低延迟共享模式，BB 语音与原生 4 声道 48kHz HD 震动再无冲突。
- **修复菜单设备切换 Bug**：修复游戏在音频设置菜单中切换 BB 输出设备时的底层寄存器漏传缺陷，选定手柄喇叭后即刻生效，无需“二次盲切”。
- **DualSense HID 扬声器路由修复（支持 Haute 4DS / USB 直连）**：拦截并修正游戏发给手柄的 HID 输出报告，在「跟随游戏」模式和 USB 有线直连时自动开启手柄喇叭，且智能兼顾 3.5mm 耳机插入检测。
- **轻量独立**：纯 C++17 编写，不依赖任何第三方运行库，解压即用。

---

### 底层技术原理解析

1. **缺陷一（核心冲突）：`PadAudioPlayback` 强制独占锁（`RVA 0x19B6610`）**
   - 游戏的 HD 触觉模块（`PadAudioPlayback`）硬编码以 `AUDCLNT_SHAREMODE_EXCLUSIVE`（独占模式）打开手柄声卡，只往后置声道（`ch2/ch3` 马达）写波形，前置声道（`ch0/ch1` 喇叭）填零。
   - 导致 Wwise 音频引擎以共享模式向手柄前置声道写 BB 哭声时直接被系统拒绝（`0x8889000A`），只能被迫静音或退回主音箱；而在 Windows 里强关独占会导致触觉模块直接崩溃，彻底失去 HD 震动。
   - **修复原理**：挂钩 `ole32.dll!CoCreateInstance`，将触觉独占流桥接为低延迟 WASAPI 共享模式（自动完成 16-bit PCM 转 32-bit 浮点与 10ms 周期同步）。Windows 混音器在 48kHz 下自动将 BB 语音与 HD 触觉合流输出。

2. **缺陷二：切换 BB 设备的寄存器漏传 Bug（`RVA 0x1DD4A00`）**
   - 官方在 `UpdateSubOutput` 切换输出设备分支中，漏写了 `mov r8d, ebx` 指令，导致传入 Wwise 的永远是上一次的旧设备 ID，引发“选音箱手柄响、选手柄音箱响”的错位现象。
   - **修复原理**：特征码定位并在内存中补全 `mov r8d, ebx`，实现菜单选择即刻生效。

3. **缺陷三：DualSense USB HID 扬声器路由缺失**
   - 官方代码在下发手柄控制包（Report `0x02`）时，未插耳机状态下仅设置了回声抑制，但将 `OutputPathSelect` 设为仅耳机（手柄喇叭处于未路由/关闭状态），且未开启喇叭音量。
   - **修复原理**：挂钩 `WriteFile` 识别手柄数据包，未插 3.5mm 耳机时自动将路由修正为 `3`（`X-X R`，右声道至手柄喇叭），音量拉至 100% 并施加 `+2` 硬件增益；插上耳机时自动恢复耳机通道。

---

### 安装步骤

1. 在本仓库的 [Releases](https://github.com/Gythiro/DualSense_BB_Audio_Fix/releases) 页面下载最新的发布包压缩包（`DualSense_BB_Audio_Fix_vX.X.zip`）。
2. 解压后将 `XAudio2_9.dll` 与 `XAudio2_9_BBFix.ini` 复制到《死亡搁浅：导演剪辑版》游戏根目录（即 `ds.exe` 所在位置）：
   - 默认 Steam 路径通常为：`steamapps\common\DEATH STRANDING DIRECTORS CUT\`
3. 也可以直接双击压缩包内的 `安装到死亡搁浅.bat` 一键自动部署。

---

### 运行环境配置确认

1. **Steam 控制器设置**：
   - 在 Steam 库中右键《DEATH STRANDING DIRECTOR'S CUT》-> **属性** -> **控制器**。
   - 将覆盖项设为 **禁用 Steam 输入**（开启 Steam 输入会屏蔽 DualSense 原生 4 声道特性）。
2. **Windows 声音控制面板（`mmsys.cpl`）**：
   - 电脑主声卡保持为 **默认设备**。
   - 手柄音频设备 `Wireless Controller` 保持启用，音量调至 100%。系统自带的“允许独占控制”保持勾选即可。
3. **适配器 / 连接方式设置**：
   - **USB 数据线直连**：即插即用。
   - **Haute 4DS / DS5Dongle 适配器**：
     - 在配置网页（[engine.haute42.com](https://engine.haute42.com)）中，**DS 扬声器模式** 设为 **跟随游戏** 或 **始终开启** 均可。
     - **扬声器增益** 建议设为 `2`。
     - **音频震动** 保持 **关闭**（确保使用纯正原生 HD 触觉）。
4. **游戏内设置**：
   - 进入 **选项 -> 控制设置**：确认 **手柄扬声器 = 开**、**震动 = 开**。
   - 进入 **选项 -> 音频设置**：将 **BB 的声音输出设备** 选为 `Wireless Controller`（即刻生效）。

---

### 配置文件说明（`XAudio2_9_BBFix.ini`）

- `EnableSharedModeHaptics=1`：开启触觉独占转共享模式（核心开关）。
- `FixBBDeviceSwapBug=1`：开启菜单设备选择错位修复。
- `FixDualSenseSpeakerRouting=1`：开启 HID 扬声器路由修正（支持跟随游戏模式）。
- `SpeakerOutputPath=3`：喇叭音频通道映射（3 = 右声道进喇叭）。
- `SpeakerVolume=100`：手柄喇叭硬件音量（61 ~ 100）。
- `SpeakerPreampGain=2`：手柄喇叭硬件增益（0 ~ 7）。
- `HapticsGain=1.0`：HD 触觉强度倍率（0.0 ~ 4.0，默认 1.0 原生强度）。
- `EnableLog=1`：是否在游戏目录输出诊断日志 `XAudio2_9_BBFix.log`。

---

### 从源码编译

本地安装 [Zig](https://ziglang.org/download/)（0.13.0 或更新版本）并加入 PATH 环境变量，双击运行工程根目录下的 `build.bat` 即可生成 `XAudio2_9.dll`。

---

### 鸣谢与参考 (Credits)

- **Kravitz**：感谢原 Nexus Mods #110 作者对游戏 WASAPI 独占锁与菜单寄存器缺陷的早期逆向发现。
- **Sony Linux Kernel Driver**：参考了 `drivers/hid/hid-playstation.c` 中关于 DualSense Output Report `0x02` 的位域与音频路由协议规范。

---

### 许可证 (License)

本项目采用 [MIT License](LICENSE) 授权许可。
