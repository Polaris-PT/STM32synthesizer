# 交接文档（给新会话 / 新同事）

> 目的：本仓库是 **STM32H750 + PCM5102A 合成器的应用层代码**，已完成阶段 1/2/3。
> 本文件把"需求、决策依据、接口、已验证事实、待办"固化下来，读完即可接手。

## 1. 需求（用户原话）
1. "在 stm32h750 上写一个合成器，通过 pcm5102 i2s 解码。你需要调查相关的开源代码，尽可能基于已有的合成器代码进行开发。**只需要开发应用层代码**，也就是合成器相关核心代码，以及 MIDI 接口(USB)。**驱动部分先由 CubeMX 生成**（这一部分在应用层代码写完之后我来做），然后再整体调试。"
2. 参考工程：`D:\Code\Embedded\STM32\workspace\H750VB\WM8978_Player`（SD 卡播放音符 wav）。
3. 后续要求：先规划再实现（已批准）；先做最小实现 + SD 音色 + 琶音器；后续"继续做采样相关代码"。

## 2. 硬件事实（已核实，含依据）
- 板子：WeAct MiniSTM32H7xx / **STM32H750VBT6**（128KB flash、1MB RAM、480MHz、25MHz HSE、8MB QSPI flash、8MB SPI flash、TF 卡、ST7735、USB OTG_FS 仅设备）。
- **音频接口 = SAI3 Block B**：**PD8=SCK/BCK、PD9=SD/DIN、PD10=FS/LRCK、PD14=MCLK** → PCM5102A。
  - 为什么不用别的：**PD4 = SDMMC1_SW（microSD 卡检测）**，所以 SAI3 Block A（要 PD4 当 FS）不可用；
    SAI1 的 MCLK=PE2、SAI2 的 PD11/PD12/PD13 都**撞板载 QSPI flash 连线**（CubeMX 允许 SAI2+QSPI 并存只是因为不知道板级连线）。
  - 依据：用户提供的外设接线图 + CubeMX 器件库 `...\STM32CubeMX\db\mcu\STM32H750VBTx.xml`。
- 时钟树（**必须与 CubeMX 一致**）：HSE 25MHz；PLL1 M=5/N=192/P=2 → SYSCLK **480MHz**，Q=20 → **USB 48.000MHz**；
  PLL2 M=5/N=39/FRACN=21076/P=8 → **24.576MHz** 给 SAI23 内核 ⇒ MCLK=12.288MHz(256fs)、MCKDIV=1 ⇒ **Fs=48.000kHz（0.00% 误差）**。
  CubeMX 判据：SAI 参数页 `Real Audio Frequency = 48.000 kHz`、`Error = 0.00%`（参考工程当年是 46.875kHz/−2.34%）。
- 其它外设：QSPI=PD13/PE2/PD12/PD11/PB2/PB6；SPIx=PB3/PB4/PD7/PD6；SDMMC1=PC8..PC12/PD2/PD4；
  ST7735=PE12/PE14/PE10/PE11/PE13/PE15；LED=PE3；USART1=PB14/PB15。
- 关键约束：**DTCM(0x20000000) 不能被 DMA1/DMA2 访问** → 音频/FX/采样缓冲都在 `.ram_section`（AXI SRAM 0x24000000）；
  音频 DMA 中断优先级必须高于 OTG_FS_IRQn。

## 3. 目录与接口（`Synth/` 是交付物，`host/` 是 PC 验证台）
| 路径 | 内容 |
|---|---|
| `Synth/include/synth/{synth.h,patch.h,pcm.h,sample.h}` | 公开 C API（不含 HAL 头） |
| `Synth/src/core/` | 波表振荡器、滤波器、包络、声部、复音分配、引擎、音色表、ROMpler |
| `Synth/src/fx/` | 过载/合唱/延迟 + 192KB FX 缓冲池 |
| `Synth/src/app/` | 琶音器、`.syn` 音色文本、`.ins` 采样乐器清单 |
| `Synth/src/engines/` | DX7 六运算器 FM（msfa）+ `.syx` 解析 |
| `Synth/port/` | **唯一接触 HAL/TinyUSB/FatFs 的地方**（上板胶水） |
| `host/` | 离线渲染 + 单元测试（mingw g++，无需硬件） |
| `docs/` | cubemx-checklist / architecture / patch-format / instrument-format / midi-cc-map |

常用命令：
```powershell
cmake -G Ninja -S host -B host/build ; cmake --build host/build
.\host\build\ssynth_tests.exe                       # 全部单测
.\host\build\ssynth_render.exe --list               # 列出内置音色
.\host\build\ssynth_render.exe --patch 12 --note 60 --seconds 3 --out fm.wav
```

## 4. 已验证事实（宿主端实测）
- 抗混叠：mip 带限波表 @2.5kHz **−121.4dB**、@C7 2093Hz **−122.6dB**（PolyBLEP 只有 −34.6dB）。
- 滤波：TPT SVF 100Hz 1.000 / 10kHz 0.0073；包络/MIDI 解析/琶音器/FX 均通过。
- FM：3 个内置 DX7 音色发声正常、八度比 2.00、释音到 0；`.syx` 压缩往返 **0/155 字节差异**；bank 解出 32 音色。
- ROMpler：多音区 + 重采样音高正确、循环持续、释音归零；`.ins` 解析/往返/错误处理通过。
- 体积（交叉编译 Cortex-M7 -O2）：自有代码 `.text` **37,455B**；加 HAL+FatFs+SAI(40-50KB)+TinyUSB(10-15KB) ≈ **88-103KB/128KB** ✓。

## 5. 待办（按优先级）
1. ~~循环接缝交叉淡化未生效~~ → **已修复（2026-10-17）**。
   - 根因**不在交叉淡化本身**，而在 `Rompler` 的取点函数 `Fetch()`：它把**小于 loop_start 的索引也向上回绕**（+循环长度），
     于是"采样开头 0..loop_start 段"实际读的是循环区内容 —— 这既让交叉淡化看起来无效，也是一处真正的播放错读
     （用相位对齐的正弦做测试时看不出来，换成"循环末尾单点尖峰"的判据后才暴露：尖峰本应在 0.354s 出现，却在 0.125s 就被听到）。
   - 修复：`Fetch()` 只把"越过循环末尾"的索引回绕回循环区；负索引夹到 0；`loop_start` 之前的样点按原样读取。
     见 `Synth/src/core/rompler.cpp` 顶部 `Fetch()`。
   - 修复后实测（`Synth/src/core/rompler.h` 的 `CrossfadeHits()` 仍保留作诊断）：
     `cf=0` 峰值 0.33664 / 跳变 0.18936（idx=16997，正好是尖峰位置）；
     `cf=256` 峰值 **0.00148** / 跳变 0.00148 —— **尖峰被压制约 47dB**，交叉淡化确实生效。
   - 该测试已作为回归用例保留：`[rom] 直接驱动 Rompler 的交叉淡化诊断`（直接驱动）与
     `[rom] 循环接缝交叉淡化 / 三次插值`（经引擎请求槽），断言 `cf=256 < cf=0 × 0.5`。
2. 采样增强：速度分层 + "最近根音"音区选择、WAV 8/24bit 支持、采样库清单扫描（`/synth/*.ins` 列表）。
3. 硬件侧（需要人）：按 `docs/cubemx-checklist.md` 生成 CubeMX 驱动 → 把 `Synth/port/synth_port_config.h` 的
   SAI 句柄名对齐（默认 `hsai3`）→ 接 CMake 片段（含 TinyUSB 的 `tinyusb_target_add` + `dcd_dwc2.c`）
   → 真机验证 QSPI 采样（放开 MPU 对 `0x90000000` 的 NO_ACCESS）、DMA/Cache 一致性、CPU 负载与 xrun 计数。

## 6. 踩过的坑（别重复踩）
- msfa：必须先调 `Exp2/Tanh/Sin/Freqlut/Env/PitchEnv/Porta/Lfo` 的 init，否则空表指针 → 0xC0000005；
  `ctrls_.core` 必须指向 `EngineMsfa` 实例；类名是 `Freqlut`。
- 弱符号在 MinGW/PE 下不工作 → 引擎用 `synth_platform_hooks_t` 钩子表。
- 测试崩溃时 stdout 缓冲会丢日志 → main 里 `setvbuf(stdout, nullptr, _IONBF, 0)`。
- 测量缓冲隔点写入 = 插零 → 频率读数减半；std::vector 越界 → STATUS_HEAP_CORRUPTION。
- C++ 成员函数名会遮蔽同名嵌套类型（`Held()` vs `struct Held`）。
- git 直连走不可用代理 → `git -c http.proxy= -c https.proxy= clone`。
- **compress 工具在本会话永久失效**（折叠基线代际错位，见 `acp_status` 的 FOLD BASE GENERATION MISMATCH）；
  解决办法是开新会话，而本文件就是为了让新会话能无缝接手。
