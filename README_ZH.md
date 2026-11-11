# 《死亡搁浅：导演剪辑版》DualSense BB 语音与 HD 触觉修复补丁

[![Platform](https://img.shields.io/badge/Platform-Windows%20x64-blue.svg)](https://github.com/Gythiro/DualSense_BB_Audio_Fix)
[![Game](https://img.shields.io/badge/Game-Death%20Stranding%20Director's%20Cut-green.svg)](https://store.steampowered.com/app/1850570)
[![License](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Nexus Mods](https://img.shields.io/badge/Nexus%20Mods-Mod%20133-orange.svg)](https://www.nexusmods.com/deathstranding/mods/133)

**[English Documentation](README.md)**

---

针对 PC 版《死亡搁浅：导演剪辑版》（Steam / Epic Games，`ds.exe`）从零编写的开源纯净修复补丁（基于原 Nexus Mods #110 作者 Kravitz 的逆向分析进行重制与深度扩展）。

彻底解决官方游戏在 PC 平台上的三大底层音频架构缺陷，实现 **手柄喇叭独立播放 BB 语音**、**原生 4 声道 48kHz HD 触觉震动反馈** 与 **自适应扳机** 完美共存，告别各种繁琐的音频配置偏方。

---

## 核心特性

- **HD 触觉与手柄语音完美共存**：无感解除 WASAPI 独占设备锁，将原生触觉流桥接为低延迟共享模式，BB 语音与原生 4 声道 48kHz HD 震动再无冲突。
- **修复菜单设备切换 Bug**：修复游戏在音频设置菜单中切换 BB 输出设备时的底层寄存器漏传缺陷，选定手柄喇叭后即刻生效，无需“二次盲切”。
- **DualSense HID 扬声器路由修复（支持 Haute 4DS / USB 直连）**：拦截并修正游戏发给手柄的 HID 输出报告，在「跟随游戏」模式和 USB 有线直连时自动开启手柄喇叭，且智能兼顾 3.5mm 耳机插入检测。
- **轻量独立**：纯 C++17 编写，不依赖任何第三方注入器或运行库，解压即用。

---

## 底层技术原理解析

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

## 安装使用指南

### 方式一：手动复制（最简单、最推荐）
1. 在 [Releases](https://github.com/Gythiro/DualSense_BB_Audio_Fix/releases) 页面或 [Nexus Mods](https://www.nexusmods.com/deathstranding/mods/133) 下载最新压缩包（`DualSense_BB_Audio_Fix_vX.X.X.zip`）。
2. 解压压缩包。
3. 直接将 **`XAudio2_9.dll`** 与 **`XAudio2_9_BBFix.ini`** 两个文件复制到《死亡搁浅：导演剪辑版》游戏根目录下（即包含 `ds.exe` 的文件夹）：
   - 默认 Steam 路径通常为：`steamapps\common\DEATH STRANDING DIRECTORS CUT\`

### 方式二：一键脚本自动安装 (`install.bat` / `安装.bat`)
1. 解压压缩包后，直接双击运行 `install.bat`（或 `安装.bat`）。脚本会自动扫描常见的 Steam 与 Epic 游戏安装目录。
2. **自定义安装目录**：如果您的游戏存放在自定义盘符或非默认路径：
   - 可以右键用记事本编辑 `install.bat`，在顶部的 `set "CUSTOM_GAME_DIR="` 填入您的游戏安装路径并保存（如：`set "CUSTOM_GAME_DIR=D:\Games\DEATH STRANDING DIRECTORS CUT"`）。
   - 或者直接双击运行脚本，当提示未自动找到时，直接将游戏文件夹拖入黑框命令行窗口中按回车即可。

---

## 运行环境配置确认

1. **Steam 控制器设置**：
   - 在 Steam 库中右键《DEATH STRANDING DIRECTOR'S CUT》-> **属性** -> **控制器**。
   - 将覆盖项设为 **禁用 Steam 输入**（开启 Steam 输入会接管手柄并屏蔽 DualSense 原生 4 声道声卡特性）。
2. **Windows 声音控制面板（`mmsys.cpl`）**：
   - 电脑主声卡（耳机或音响）保持为 **默认设备**。
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

## 配置文件说明 (`XAudio2_9_BBFix.ini`)

| 配置项 | 默认值 | 作用说明 |
|---|---|---|
| `EnableSharedModeHaptics` | `1` | 开启触觉独占转共享模式（核心开关）。 |
| `FixBBDeviceSwapBug` | `1` | 开启菜单设备选择错位修复。 |
| `FixDualSenseSpeakerRouting`| `1` | 开启 HID 扬声器路由修正（支持跟随游戏模式与 USB 直连）。 |
| `SpeakerOutputPath` | `3` | 喇叭音频通道映射（3 = 右声道进喇叭，推荐）。 |
| `SpeakerVolume` | `100` | 手柄喇叭硬件音量（61 ~ 100，默认 100）。 |
| `SpeakerPreampGain` | `2` | 手柄喇叭硬件增益（0 ~ 7，默认 2，与 Haute 网页增益一致）。 |
| `HapticsGain` | `1.0` | HD 触觉强度倍率（0.0 ~ 4.0，默认 1.0 原生强度）。 |
| `EnableLog` | `1` | 是否在游戏目录输出诊断日志 `XAudio2_9_BBFix.log`。 |

---

## 从源码编译

本地安装 [Zig 编译器](https://ziglang.org/download/)（0.13.0 或更新版本）并加入 PATH 环境变量，运行工程根目录下的 `build.bat` 即可生成 `XAudio2_9.dll`：

```cmd
git clone https://github.com/Gythiro/DualSense_BB_Audio_Fix.git
cd DualSense_BB_Audio_Fix
build.bat
```

---

## 卸载与还原

- 直接删除游戏目录下的 `XAudio2_9.dll`、`XAudio2_9_BBFix.ini` 与 `XAudio2_9_BBFix.log` 即可。
- 或双击运行 `uninstall.bat`（或 `卸载.bat`）自动清理并恢复官方纯净状态。

---

## 鸣谢与参考 (Credits)

- **Kravitz**：感谢原 Nexus Mods #110 作者对游戏 WASAPI 独占锁与菜单寄存器缺陷的早期逆向发现。
- **Sony Linux Kernel Driver**：参考了 `drivers/hid/hid-playstation.c` 中关于 DualSense Output Report `0x02` 的位域与音频路由协议规范。

---

## 许可证 (License)

本项目采用 [MIT License](LICENSE) 授权许可。
