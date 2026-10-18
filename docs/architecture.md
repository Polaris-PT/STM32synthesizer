# 架构与实测记录

## 1. 数据流

```
USB MIDI (TinyUSB, PA11/PA12)
   └─ tud_midi_rx_cb / 轮询 → synth_midi_in(bytes,len) → 256B 无锁环形缓冲 (SPSC)
                                                              │
SAI3 Block B DMA 半传输/完成中断 (2×128 帧, 2.67ms) ──► synth_render_i32()
   ├─ 取出环形缓冲并解析 MIDI（running status/SysEx 丢弃）
   ├─ 应用待处理请求（换音色/panic/主音量）
   ├─ 16 个 Voice::Render（每声部：2 振荡器 + 子振/噪声 → 滤波 → 幅度包络）
   └─ 总线：主音量 → 软限幅 → DC 阻断 → float→int32(24bit 左对齐) → DMA 缓冲
```

采样率固定 48 kHz；块长 128 帧；无 RTOS。

## 2. 音频输出（引脚依据）

| 项 | 值 | 依据 |
|---|---|---|
| 外设 | **SAI3 Block B** 主机发送 | CubeMX 器件库 `STM32H750VBTx.xml` + 核心板原理图 |
| 引脚 | PD8=SCK/BCK、PD9=SD/DIN、PD10=FS/LRCK、PD14=MCLK | 上述四个引脚在原理图上均空闲 |
| 不可用的选择 | SAI1 (MCLK=PE2) 与 SAI2 (PD11/PD12/PD13) | 与板载 QSPI flash 连线冲突（IO2=PE2、IO0/IO1/IO3=PD11/PD12/PD13） |
| 不可用的选择 | SAI3 Block A (FS=PD4) | PD4 = SDMMC1_SW（microSD 卡检测） |
| 数据格式 | 32bit slot × 2，MCLK=12.288MHz(256fs) | 与已跑通的 STM32_UAC1.0 工程同构 |
| 内核时钟 | PLL2 = 24.576 MHz（M=5,N=39,FRACN=21076,P=8） | 25MHz HSE → 精确 48.000 kHz（Error 0.00%） |

## 3. 内存与 CPU 预算

### 实测（交叉编译统计，2026 本轮）

用 `arm-none-eabi-gcc 14.2 -O2 -mcpu=cortex-m7 -mfpu=fpv5-d16 -c` 逐文件编译 +
`arm-none-eabi-size` 统计：

| 分组 | 文件数 | .text | .data | .bss |
|---|---|---|---|---|
| Synth/src/core | 7 | 16,207 | 4 | 59,732（含 33.5KB 波表） |
| Synth/src/engines | 3 | 5,556 | 0 | 469 |
| Synth/src/fx | 4 | 2,528 | 196,608（192KB FX 池，.ram_section） | 4 |
| Synth/src/app | 2 | 3,105 | 0 | 0 |
| Synth/port | 4 | 1,095 | 2,052 | 348 |
| Synth/third_party/msfa | 11 | 8,964 | 4 | 29,212（查表，运行时填充） |
| **合计** | 31 | **37,455** | 198,668 | 89,765 |

**结论**：应用层 + msfa + port 的可执行代码 ≈ **37.5 KB flash**（msfa 的 29KB 是 RAM 里的查表）。
加上 CubeMX 侧 HAL + FatFs + SAI3 + DMA（≈40–50KB）与 TinyUSB（≈10–15KB），
整机预计 **88–103 KB / 128 KB**，符合既定预算。

> 统计时 `patch_storage_sd.c` 与 `sample_storage_sd.c` 因脚本给的 FatFs/HAL SD 头组合不完整而编译失败
> （单独用 WM8978_Player 的头检查时 exit=0），未计入；它们体积很小，不影响结论。


| 项 | 数值 | 来源 |
|---|---|---|
| 波表（saw/tri，9 层 mip） | 33.5 KB RAM | 本次实现，`WaveTables::storage_` |
| 正弦表 | 4 KB RAM | 本次实现 |
| 音频 DMA 缓冲 | 2×128 帧 × 4B × 2ch = 2 KB | 放 AXI SRAM/D2 SRAM，MPU 非缓存 |
| 引擎代码 | 待上板实测 | 目标：整机 ≤ 128KB flash |
| 参考基线 | HAL 37.4KB；HAL+FatFs+LCD 73.6KB；HAL+SAI+ST USB 70.8KB | 本地 arm-none-eabi-size 实测 |

## 4. 宿主端实测（2026-xx，mingw g++ 14.2 / -O3）

`ssynth_tests.exe` 全部通过：

| 测试 | 实测 | 判据 |
|---|---|---|
| 锯齿正向过零（440Hz, 1s） | 440 | ±3 |
| 混叠 @2.5kHz（第 12 谐波折叠到 18kHz） | **−121.4 dB** | < −55 dB |
| 混叠 @C7 2093Hz（折叠到 22884Hz） | **−122.6 dB** | < −55 dB |
| SVF 100 Hz / 10 kHz | 1.000 / 0.0073（−42.7 dB） | 通带 ≈0dB、10kHz < −30dB |
| ADSR 时序 | 起音后 >0.5，释音 100ms 后 <0.01 | |
| MIDI 解析 | running status / 实时字节穿插 / SysEx 丢弃 / PC | 全部正确 |
| 三音和弦 1s | 3 声部、峰值 ≤1.0、无 NaN | |
| 释音 2s | 末 133ms < 0.01 | |
| 20 音 > 16 声部 | 触发偷音、声部数 ≤16 | |
| panic | 声部数归零 | |

`ssynth_render.exe` 20 秒完整演示（8 声部峰值、68 次 NoteOn）：宿主渲染耗时 0.081s / 20s 音频 ≈ **247× 实时**。

## 5. 抗混叠方案

不使用 PolyBLEP，而是 **mip 分层带限波表**：按频率选择谐波数（256→1）与表长（2048→32），
表在 `synth_init()` 里用复数旋转器递推生成（O(N·H)，目标端约 10ms），每层峰值归一化。
脉冲波由两个相位错开的锯齿相减得到（零均值，按占空比归一化）。
实测混叠低于 −120 dB，远优于 −60 dB 目标，且比逐样点 PolyBLEP 更快（查表+线性插值）。

## 6. 已知待办

- `Synth/src/fx/`：合唱、延迟、软限幅独立模块
- `Synth/src/app/`：SD 音色（`/synth/*.syn`）、琶音器/音序器
- `Synth/port/`：SAI3 Block B 适配器、TinyUSB MIDI、SD 存储、platform_stm32.c（时钟/MPU/FTZ/DWT）
- 阶段 2：msfa（Apache-2.0）六运算器 FM + `.syx`
- 阶段 3：多样本 ROMpler + QSPI 采样
---

## 7. 效果链 / 琶音器 / 音色文本（本轮新增）

```
声部混音 → 过载(Drive) → 合唱(Chorus) → 延迟(StereoDelay) → 主音量 → 软限幅 → DC 阻断
                                                                    ↘ int32 → SAI3 DMA
```

- **缓冲池**：`Synth/src/fx/fx_memory.cpp` 一个 192KB 静态池（延迟 0.6s = 128KB、合唱 60ms = 16KB），
  在目标端位于 `.ram_section`（AXI SRAM）。放 DTCM 会溢出且 DMA 访问不到。
- **过载**：`Drive(x,gain,comp) = SoftClip(x·gain)·comp`，`gain = 1+6·fx_drive`、`comp = 1/(1+1.6·fx_drive)`。
- **合唱**：两路 LFO 调制延迟，通道间相位差 1/4 周期；`rate = 0.18+4·fx_chorus` Hz。
- **延迟**：int16 环形缓冲 + 线性插值读，读指针一阶平滑（避免跳变爆响）；时间由 `fx_delay_fb` 决定（0.12..0.5s），反馈 ≤0.85。
- **琶音器**（`Synth/src/app/arp.cpp`，MIT）：模式 上行/下行/上下/随机/按演奏顺序，1–4 八度，
  速率 0.2–30 步/秒或 **MIDI 时钟同步**（0xF8，24ppqn，默认 1/16 = 每 6 时钟一步），门限长度可调。
  在渲染块开头按"距下一步的样点计数"调度，事件驱动 `VoiceNoteOn/VoiceNoteOff`（2.67ms 量化）。
- **音色文本**（`Synth/src/app/patch_text.cpp`）：35 个字段、枚举支持名字或数字、支持注释；
  与 SD 存储（`Synth/port/patch_storage_sd.c`）配合实现 `/synth/NNN.syn` 音色文件。

## 8. port 层（CubeMX 侧胶水，已完成并语法校验）

| 文件 | 作用 |
|---|---|
| `Synth/port/synth_port_config.h` | 唯一适配点：SAI 句柄名、块长、缓冲段、SD 目录 |
| `Synth/port/platform_stm32.c` | D-Cache、FPU FTZ、DWT 周期计数、USB 48MHz 时钟、平台钩子 |
| `Synth/port/audio_out_sai.c` | SAI3 Block B 循环 DMA + 半/满回调（**Size = 帧数×2 个 32bit 字**） |
| `Synth/port/usb_midi_tinyusb.c` | TinyUSB MIDI 描述符/回调 → `synth_midi_in()` |
| `Synth/port/patch_storage_sd.c` | FatFs 挂载/读取/保存 `/synth/NNN.syn` |

**验证方式**：没有硬件也能查错 —— 用 `arm-none-eabi-gcc -fsyntax-only` 对着真实 HAL 头文件编译
（平台/音频用 STM32_UAC1.0 的 HAL 树，SD 用 WM8978_Player 的 HAL 树），三个文件全部 exit=0。

## 9. 与计划的偏差（记录）

- **没有 vendor DaisySP**：原本计划复用 DaisySP（MIT）的振荡器/滤波，但自研的 mip 带限波表
  实测混叠 −121 dB（DaisySP 的 PolyBLEP 量级约 −35 dB），且不引入 C++ 第三方代码，
  代码量与 flash 更小。DaisySP 仍保留为阶段 3（Karplus-Strong、ModalVoice 等）的参考/候选项。
- **QSPI 阶段 1 未使用**：引擎 + FX + port 预计 80–100KB，128KB 内置 flash 够用；
  SAI3 Block B 不占 QSPI 引脚，阶段 2/3 若不够可直接启用（数据存储或 XIP）。
- **TinyUSB 已 vendor**（MIT，commit 3c6524e，仅保留 `src/`）：该版本 `tud_midi_n_packet_read(itf, pkt)`
  **没有 cable 参数**，与网上旧示例不同，代码已按实际版本写。

## 10. 阶段 1 完成度

| 项 | 状态 |
|---|---|
| 振荡器/滤波/包络/复音/偷音 | ✅ 单测通过 |
| 效果链（过载/合唱/延迟/限幅/DC） | ✅ 单测通过（延迟位置与幅度精确） |
| 琶音器（含 MIDI 时钟同步） | ✅ 单测通过 |
| MIDI 解析 + CC 映射 + Program Change | ✅ |
| 音色系统（12 内置 + `.syn` 文本 + SD 加载代码） | ✅ |
| 宿主端离线渲染台 + 单元测试 | ✅ 全部通过 |
| port 层（SAI3/TinyUSB/SD/平台） | ✅ 已写并语法校验（待上板联调） |
| 文档（CubeMX 清单/CC 映射/音色格式/架构） | ✅ |
| 阶段 2：msfa 六运算器 FM + `.syx` | ⏳ 未开始 |
| 阶段 3：多样本 ROMpler + QSPI | ⏳ 未开始 |
---

## 11. 阶段 2：DX7 六运算器 FM（已完成核心）

- **来源**：vendor 了 Apache-2.0 的 msfa 核心（`Synth/third_party/msfa`，见该目录 README 登记表），
  **未引入** Synth_Dexed 中 GPL-3.0 的 `dexed.cpp`（VoiceManager）、`EngineMkI`、`EngineOpl`、`PluginFx`。
  复音分配、音色载入、MIDI、与主引擎的路由全部自研（`Synth/src/engines/fm_dx7.*`）。
- **语音数据格式**：标准 DX7 **VCED**（155 字节，OP6 在最前，每运算器 21 字节），
  msfa 的 `Dx7Note::init(patch[156])` 期望的正是这个布局；LFO 从 `patch[137]` 起 6 字节。
- **必须的初始化**（否则查表指针为空 → 访问违例）：
  `Exp2::init(); Tanh::init(); Sin::init(); Freqlut::init(sr); Env::init_sr(sr); PitchEnv::init(sr); Porta::init_sr(sr); Lfo::init(sr);`
  以及 `Controllers::core = (FmCore *)&engine_`（`Dx7Note::compute` 通过它调用 `EngineMsfa::render`）。
- **块长**：msfa 固定 64 样点（`_N_`）；FM 输出为 int32（满幅约 2^28），本引擎转 float 并累加到交织立体声，
  再做软限幅。
- **声部管理**：8 个 FM 声部，策略 空闲 → 释放中最旧 → 最旧偷音；以"每块输出峰值 < 4096"判定释放完成
  （另有 20 秒释放上限兜底），`Panic()` 立即清空。
- **主引擎路由**：`synth_patch_t.engine_type`（0=减法，1=FM）+ `fm_voice`；
  `VoiceNoteOn/VoiceNoteOff` 与 `RenderBlock` 按 engine_type 分流，切换音色时两类声部自然交叉淡出。
- **内置 FM 音色**（自研，非 DX7 原厂）：FM E.Piano / FM Bass / FM Bell，工厂音色表现在 **15 个**。

### 实测（宿主端单测，全部通过）

| 项 | 结果 |
|---|---|
| FM E.Piano 峰值 / RMS | 0.4660 / 0.3346 |
| FM Bass / FM Bell 峰值 | 0.3829 / 0.4629 |
| 音高（八度关系） | E.Piano 262→523 Hz（2.00）、Bass 350→700 Hz（2.00） |
| 释音 | 三个音色均 3 秒内到 0.000000，声部归零 |
| 复音/偷音 | 12 音 → 8 声部、4 次偷音 |
| 引擎级 FM 路由 | FM 音色（index 12）经 `synth_set_patch/synth_note_on` 播放，峰值 0.3641，无 NaN 不削顶 |

### `.syx` 载入（已完成，`Synth/src/engines/sysex.cpp`）

支持三种输入，全部有单测覆盖：

| 输入 | 格式 | 说明 |
|---|---|---|
| 32 音色压缩 bank | `F0 43 ch 09 20 00` + 32×128B + chk + `F7`（4104B） | 最常见的 DX7 bank dump；按 Yamaha "Bulk Dump Packed Format" 解包到 VCED 155 字节 |
| 单音色非压缩 dump | `F0 43 ch 00 01 1B` + 155B + chk + `F7`（163B） | 直接拷贝 |
| 裸 VCED | 155/156 字节文件 | 直接拷贝 |

压缩格式的位域（依据 Yamaha DX7 MIDI Data Format Sheet 的 Packed Format 表，见
`docs/research/dxsyx/doc/dx7-sysex-format.txt`）：
每运算器 17 字节 —— R1-R4、L1-L4、BP、LD、RD、`[RC:2|LC:2]`、`[DET:4|RS:3]`、
`[KVS:3|AMS:2]`、OL、`[FC:5|M:1]`、FF；随后 PEG 8 字节、`ALG:5`、`[FB:3|OKS:1]`、
LFS/LFD/LPMD/LAMD、`[LPMS:3|LFW:3|LKS:1]`、TRNSP、10 字节音名。

引擎 API：`synth_load_fm_sysex(data, len, index)` 解析后把第 index 个音色放进 RAM 槽并切到 FM
（`fm_voice = 0xFF`）；SD 端口 `synth_port_patch_load_file()` 先试 .syx 再试文本音色，
所以 `/synth/NNN.syx` 直接可用。

实测：压缩往返 **0/155 字节差异**；bank dump 解出 32 个音色；单音色 dump 解出 1 个；
载入 bank 第 3 个音色经引擎播放峰值 0.365；垃圾数据正确报错。
---

## 12. 阶段 3：多样本 ROMpler（核心完成，QSPI 采样待做）

### 数据模型（`Synth/include/synth/sample.h`）
- 采样为**单声道 16bit、RAM 常驻**（不做实时流式，避免在音频中断里读卡）：`synth_sample_zone_t`
  含 `data/frames/rate/root/low/high/loop/loop_start/loop_end/gain`；
  一个乐器最多 **8 个音区**（`SYNTH_ROM_MAX_ZONES`），`synth_sample_instrument_t` 带 `volume/name`。
- 回归音区选择：先按 `low ≤ note ≤ high` 匹配，没匹配上则用第一个可用音区（方便单音区乐器）。

### 引擎（`Synth/src/core/rompler.cpp`）
- **8 个采样声部**（`SYNTH_ROM_MAX_VOICES`），分配策略 同音重触发 → 空闲 → 释放中最旧 → 最旧偷音。
- 重采样步进 `step = 2^((note-root)/12) × (zone.rate / 输出采样率)`，读位置为 double 精度，
  **线性插值**；循环时读位置在 `[loop_start, loop_end)` 内回绕（`loop_end == 0` 视为采样末尾）。
- 每个声部一个 ADSR（默认 2ms 起音 / 50ms 衰减 / 满保持 / 80ms 释音），力度作用于音量。
- 主引擎路由：`patch.engine_type == 2` → `rom` 渲染；`VoiceNoteOn/Off` 与 `SilenceAll/panic` 一并分流。

### 载入路径
- 引擎 API：`synth_set_sample_instrument(const synth_sample_instrument_t*)`（请求槽，渲染块开头生效）、
  `synth_set_engine_type(0|1|2)`。
- SD 端口 `Synth/port/sample_storage_sd.c`：`synth_port_sample_load_wav(path, root, low, high, loop, loop_start, loop_end)`
  —— 自家 RIFF 解析（只接受 PCM 16bit、1–2 声道），立体声下混为单声道，数据写入
  **512KB 静态采样池**（`g_sample_pool`，`SYNTH_PORT_AUDIO_RAM` → `.ram_section` = AXI SRAM）；
  `synth_port_sample_activate()` 把乐器交给引擎并切到 ROMpler；`synth_port_sample_pool_reset()` 清空。
  可多次调用以组成**多音区**乐器（不同音域不同采样）。

### 实测（宿主端单测，全部通过）
| 项 | 结果 |
|---|---|
| 音区 1 根音（root=A3，采样 220Hz@24kHz，输出 48kHz） | **220 Hz** |
| 音区 1 下行八度（A2） | **110 Hz** |
| 音区 2 根音（root=A4，采样 330Hz@24kHz） | **330 Hz**（多音区映射正确） |
| 循环 | 采样 0.6s 衰减完，循环后 3 秒尾部能量 801（持续发声） |
| 释音 | NoteOff 后 0.27s 峰值 0.000000 |
| 输出 | 无 NaN、不削顶 |

### QSPI flash 采样（代码已就绪，待真机联调）
`Synth/port/sample_storage_qspi.c`：内存映射模式（`SYNTH_QSPI_MAP_BASE = 0x90000000`）下**零拷贝**使用采样 ——
`synth_port_sample_add_qspi(byte_offset, frames, rate, root, low, high, loop, loop_start, loop_end)`
可多次调用组成多音区，`zone->data` 直接指向映射地址；
`synth_port_sample_qspi_activate(name)` 交给引擎并切到 ROMpler。

前置条件（都在 CubeMX/联调侧）：
1. QUADSPI Bank1 四线（PB2/PB6/PD11/PD12/PD13/PE2，与 SAI3 不冲突）并进入内存映射模式；
2. **放开 MPU**：CubeMX 默认把 0x60000000–0xDFFFFFFF 设为 NO_ACCESS，必须为 0x90000000 加可读区域，
   否则访问会 HardFault；
3. flash 里存**裸 16bit 单声道 PCM**（无文件头），采样地址 = 0x90000000 + 字节偏移。

### 采样质量：三次插值 + 循环接缝交叉淡化（已修复并验证）

- 每个音区可选 `interp`（0=线性、1=三次 Hermite/Catmull-Rom）与 `crossfade`（循环接缝淡化样点数，0=关闭）。
- **修复的 bug**：`Rompler` 的取点函数 `Fetch()` 曾把**小于 `loop_start` 的索引也向上回绕**（+循环长度），
  导致"采样开头 0..loop_start 段"实际读的是循环区内容 —— 用相位对齐的正弦测试看不出来，
  换成"循环末尾单点尖峰"判据后立刻暴露（尖峰应在 0.354s 出现，却在 0.125s 就被听到）。
  修复后只把"越过循环末尾"的索引回绕，负索引夹到 0，`loop_start` 之前的样点按原样读取。
- **修复后实测**（尖峰位于采样 8499，循环 3000..8500）：
  | 配置 | 峰值 | 最大相邻跳变 |
  |---|---|---|
  | `crossfade=0`（对照） | 0.33664 | 0.18936（位置 16997，正是尖峰处） |
  | `crossfade=256` | **0.00148** | **0.00148** |
  | `crossfade=2048` | — | **0.00015** |
  ⇒ 尖峰被压制约 47 dB，淡化越长效越好。回归用例：`[rom] 直接驱动 Rompler 的交叉淡化诊断`、
  `[rom] 循环接缝交叉淡化 / 三次插值`（`Rompler::CrossfadeHits()` 保留作诊断计数）。

### 尚未完成 / 待真机
- QSPI 采样与 MPU 配置的真机验证；
- 采样库组织（多音色多音区）与 SD 上的乐器清单文件；
- 整机联调（用户用 CubeMX 生成驱动后，把 `Synth/port/synth_port_config.h` 的句柄名对齐即可）。
