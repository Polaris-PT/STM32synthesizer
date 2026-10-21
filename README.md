# STM32H750 合成器（PCM5102A / USB-MIDI / QSPI 采样库 / TFT UI）

在 **WeAct MiniSTM32H7xx（STM32H750VBT6，128 KB flash / 1 MB RAM）** 上跑的实时合成器：
C++17 复音引擎 + USB-MIDI 设备 + QSPI 采样库 + SD 音色/状态持久化 + ST7735 TFT UI
（UI 作为 **RAM overlay 模块**运行，不占内部 flash）。

```
USB-MIDI ──► MIDI 解析 ──► 引擎（减法/FM/采样，10~16 复音）
                                 ├─► 混响/延迟/合唱（FX 链）
                                 └─► SAI3 Block B ──► PCM5102A ──► 喇叭
采样音源 ── SD 卡 /synth/samples.bin ── 导入 ── QSPI 8 MB ──► 零拷贝播放
音色/状态 ── SD 卡 /synth/*.syn、state.cfg ── 开机恢复
TFT UI   ── QSPI 存镜像 ── 上电拷到 AXI SRAM 执行（PE10~PE15，PC13 翻页）
```

## 硬件

| 部件 | 连接 |
|---|---|
| 音频 DAC | PCM5102A：**SAI3 Block B**，PD8=SCK、PD9=SD、PD10=FS、PD14=MCLK（12.288 MHz） |
| USB | USB-C 直连 PA11/PA12（TinyUSB，device FS，枚举成 USB-MIDI 设备） |
| 存储 | microSD（SDMMC1 4-bit：PC8~PC12 + PD2）+ 板载 QSPI flash（W25Q64，8 MB） |
| 屏幕 | ST7735 1.8" 128×160：SPI 位敲，PE12=SCK、PE14=MOSI、PE11=RST、PE13=DC、PE15=CS、PE10=背光 |
| 按键 | K1 = PC13（**按下为高**，内部下拉） |
| 调试 | USART1 115200（PB14/PB15）打印日志 |

时钟：HSE 25 MHz → PLL1 480 MHz 内核 / HCLK 240 MHz / **PLL1Q = 48 MHz（USB）**；
PLL2 M5/N108/FRACN1101/P22 → 24.576 MHz 供 SAI3，`MCKDIV=2` → MCLK 12.288 MHz → **48.000 kHz 精确**。

## 构建与烧写（Windows + ARM GCC）

依赖：`arm-none-eabi-gcc` 14.2、CMake ≥ 3.22、Ninja 或 MinGW Make、OpenOCD（CMSIS-DAP）、Python 3。

```powershell
# 日常调试版（串口日志全开）
powershell -File tools\build.ps1 -Generator 'MinGW Makefiles'

# 发布版（关日志 + 冷路径 -Os，省 ~6 KB flash）
powershell -File tools\build.ps1 -Trim -Generator 'MinGW Makefiles'

# 构建并烧写
powershell -File tools\build.ps1 -Generator 'MinGW Makefiles' -Flash

# 读板子状态（时钟/故障位/复音/SD/模块/…，一条命令）
powershell -File tools\status.ps1
```

VS Code 里 `Ctrl+Shift+B` 有对应任务（构建 / 下载 / 读状态 / **上传 UI 模块**）。
CMake 用户也可以直接用 `CMakePresets.json`（内置 `cmake/gcc-arm-none-eabi.cmake` 工具链文件）。

## 演奏与调试工具

```powershell
powershell -File tools\play.ps1 -List                       # 找到 "H750 PCM5102A Synth"
powershell -File tools\play.ps1 -Patch 20 -Notes "48,52,55,60"   # 采样钢琴和弦
python tools\pack_samples.py --max-seconds 3.0 --rate 32000 # 把 samples/ 打包成 samples.bin
python tools\upload_module.py                              # 改完 UI 一条命令烧进 QSPI（不用拔卡）
```

## 仓库结构

| 路径 | 说明 |
|---|---|
| `Synth/include/synth/` | 公开 API（不依赖 HAL） |
| `Synth/src/core/` | 振荡器、滤波器、包络、声部、复音分配、引擎、FM |
| `Synth/src/midi/` | MIDI 1.0 字节流解析（含 SysEx 丢弃、系统公共消息） |
| `Synth/src/fx/` | 合唱 / 延迟 / 软限幅 |
| `Synth/src/app/` | 音色库、`.syn`/`.ins` 文本解析、琶音器 |
| `Synth/port/` | **唯一接触 HAL / TinyUSB 的地方**（音频 DMA、QSPI、SD、USB-MIDI、日志） |
| `Synth/module/` | RAM overlay 模块示例（ST7735 TFT UI） |
| `Core/`、`Drivers/`、`FATFS/`、`Middlewares/` | CubeMX 生成的驱动代码 |
| `host/`、`tests/` | PC 端离线渲染台与单元测试（不需要硬件） |
| `tools/` | 构建/烧写/读状态/打包/上传/挑音脚本 |
| `docs/` | 详细文档（见下） |

## 文档

| 文件 | 内容 |
|---|---|
| `docs/cubemx-checklist.md` | CubeMX 里每个外设怎么配 + 验收判据 |
| `docs/midi-map.md` | 音色号 / CC / NRPN 完整映射 |
| `docs/sample-library.md` | QSPI 采样库格式、打包器、上板流程 |
| `docs/overlay.md` | RAM overlay 模块（USB 上传、ABI、踩过的坑） |
| `docs/code-map.md` | 代码结构导读（先读哪个文件） |
| `docs/architecture.md` | 音频路径、引脚依据、内存预算 |
| `docs/phase2-review.md` | 生成代码审查、HardFault/对齐/SD 打通的完整记录 |
| `docs/perf-notes.md` | 性能实测与优化（复音过载、渲染搬出中断） |
| `docs/handoff.md` | 交接清单与"踩过的坑"总表 |

## 宿主端（PC）离线验证：不需要硬件

```powershell
cmake -G Ninja -S host -B host/build
cmake --build host/build
./host/build/ssynth_render.exe --demo --patch 0 --out demo.wav
./host/build/ssynth_tests.exe
```

## 已知限制

- 内部 flash 只剩几百字节（调试版 99.8%）→ 大功能请走 **RAM overlay 模块**（`docs/overlay.md`）。
- FM / 采样引擎各只有**一个实例**、ROMpler 最多 8 区 ⇒ 多通道音色映射时"最后切换的音色"生效。
- 效果链是**全局一条**（跟随最后一次切换的音色）。
- 采样库受 QSPI 8 MB 与 128 KB RAM 采样池限制（`docs/sample-library.md`）。
- 仓库**不含采样音源**（`samples/` 被忽略：体积大、且多为第三方授权）；用
  `tools/pick_samples.py` + `tools/pack_samples.py` 自行准备。

## 第三方组件与许可

| 组件 | 许可 |
|---|---|
| STM32 HAL / CMSIS（`Drivers/`、`Middlewares/`，CubeMX 生成） | BSD-3-Clause / Apache-2.0 |
| FatFs R0.12c（`FATFS/`、`Middlewares/Third_Party/FatFs`） | ChaN 的自由软件许可（允许商用与再分发） |
| TinyUSB（`Synth/third_party/tinyusb`） | MIT |
| music-synthesizer-for-android 的 FM 内核（`Synth/third_party/msfa`） | Apache-2.0 |
| ST7735 初始化序列 / ASCII 字模 | 参考 WeAct SDK 示例工程改写 |

各处均保留原始许可文件（`LICENSE`/`LICENSE-APACHE2.txt`）。本仓库自身代码的许可见 `LICENSE`。
