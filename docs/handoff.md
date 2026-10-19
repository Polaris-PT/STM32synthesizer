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
  PLL2 M=5/N=108/FRACN=1101/P=22 → **24.57599987MHz** 给 SAI23 内核（−0.13Hz ≈ −5ppb）
  ⇒ MCLK=12.288MHz(256fs)、**MCKDIV=2** ⇒ **Fs=48.000kHz（0.00% 误差）**。
  **判据**：`SAI_CK` 必须是 **12.288MHz 的整数倍**（NODIV=0 时 `Fs = SAI_CK/(MCKDIV×256)`），
  所以 48MHz 之类的时钟只能得到 46.875kHz（−2.34%）；24.576MHz 只能靠 FRACN 逼近
  （FRACN 13bit，合法 0..8191；本文档早期写的 `N=39/FRACN=21076/P=8` 是**无效值**，已改正）。
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
- 体积（**实机固件，2026-10-19 实测**，SD+FATFS 已启用）：
  - 默认构建（串口日志开，日常调试用）：**FLASH 124,792 B / 128 KB = 95.21%**（`build\RelWithDebInfo`）
  - 发布构建 `tools\build.ps1 -Trim`（关调试日志 + 冷路径 -Os）：**FLASH 120,072 B = 91.61%**（`build\release`）
  - 其它区两版相同：AXI RAM 400,800 B (76.45%)、DTCMRAM 46,336 B (35.35%)
  - ⇒ 发布版留 **~10.7 KB** flash 余量，**QSPI XIP（把 FatFs/TinyUSB 搬到外部 flash）暂时不必做**。
  - 两个版本都已上板验证：发布版 `-Trim` 实测 `fault = 0`、`usb_mounted = 1`、`audio_ck = 24.576 MHz`、
    `sai_mckdiv = 2`、`qspi_id = 0xEF4017`、发 4 组 note on/off 后 `midi_rx_bytes = 1218`、`cpu_permille = 232‰`、
    `audio_underruns = 0`、`status.ps1` 结论 `✓ 固件侧一切正常`。
- **LTO 试过并放弃**（`-flto` 能到 116,172 B / 88.63%，省 ~4 KB，但和 HAL 的弱符号覆盖模式冲突）：
  HAL 里 `HAL_*_MspInit()` 是 `__weak` 空实现、强定义在我们的 `Core/Src/*.c`；整程序 LTO 时 GCC 让库里那个
  弱版本"胜出"，ELF 里 `HAL_SAI_MspInit` 符号整个消失 → 实机 `fault = 0x126`（SAI 时钟/SAI DMA/AUDIO_START/QSPI_MM
  四个故障位全亮）、没声音、QSPI 映射失败。给 HAL 库加 `-fno-lto` 能修好（119,928 B），但省下的只剩 ~144 B，不值得；
  给 `Core/Src` 加 `-fno-lto` 无效（目标级 `-flto` 在命令行上排得更靠后）。所以 **不提供 LTO 开关**。
  将来若要重试：`App_HardFaultRecordC` 需要 `__attribute__((used, externally_visible))`（naked 汇编按符号名调用它）。
- 发布构建 `tools\build.ps1 -Trim -Flash` 的两个坑：
  ① `status.ps1` 靠 `arm-none-eabi-nm` 按名字读内存，发布版里没有 C 读者的计数器会被 `--gc-sections` 回收
     （现象：`ELF 里找不到符号 g_synth_audio_xruns`）→ `Core/Src/main.c` 的 `App_KeepObservablesAlive()` 读一遍保活，
     以后新增"给外部工具看"的变量记得加进去；
  ② 发布版**没有串口日志**（`SYNTH_PORT_DEBUG_UART=0`），读状态要指向同一份 ELF：
     `tools\status.ps1 -Elf build\release\STM32synthesizer.elf`。调试期仍用默认构建（有日志）。

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
3. ~~硬件侧接入（句柄名/CMake/MPU/main.c）~~ → **已完成（2026-10-18）**，见 [phase2-review.md](phase2-review.md)：
   `Core/Src/main.c` 已重构（SAI3 时钟走 PLL2、32bit、DMA 兜底、MPU 放开 `0x90000000`、启动序列与故障位图），
   `Synth/port/synth_port_config.h` 句柄名已对齐 `hsai_BlockB3`，根 `CMakeLists.txt` 已接引擎 + TinyUSB，
   链接脚本已加 `.ram_section`。**实测 72/72 源文件编译通过、链接成功**。
   CubeMX 侧同类项也已完成：`.ioc` 里 SAI3 = 32bit + DMA1_Stream0 循环 WORD + NVIC 0、SAI23 时钟源 = PLL2
   （M5/N108/FRACN1101/P22 = 24.57599987 MHz）、QUADSPI 参数（48 MHz / FlashSize 23 / HALF_CYCLE / CS 3 cycle）、
   SDMMC1 + FATFS、PD4 卡检测上拉、PLL1Q = 20（USB 48 MHz）、CODE_PAGE 850 + LFN（静态缓冲）。
4. **应用集成收尾已完成（2026-10-18 晚）**：
   - `main()` 手写在 `Core/Src/main.c` 的 `USER CODE 4`（`.ioc` 勾了 "Do not generate the main()"），
     含 `MPU_Config() → 时钟 → App_ZeroDtcmBss() → MX_*_Init() → 音频 → QSPI 内存映射 → 引擎 → 主循环`。
   - **QSPI 进内存映射**：`App_QspiEnterMemoryMapped()`（复位 + JEDEC ID 自检 + QE 置位 + `0xEB` 四线快读
     + `HAL_QSPI_MemoryMapped()`）；`g_app_status.qspi_id` 应为 `0xEF4017`。
   - **内存策略调整**：`.data/.bss` 默认搬到 AXI SRAM，DTCM 只留栈/堆 + `SYNTH_HOT` 热点表
     （正弦表 4 KB + 波表 32 KB 已搬进 `.dtcm_bss`，由 `App_ZeroDtcmBss()` 清零）。
   - 实测（RelWithDebInfo，SD 关闭）：FLASH 103,280 B (78.8%)、DTCM 46,336 B (35.4%)、AXI 256,060 B (48.8%)
     —— 其中约 8~11 KB 是诊断代码与串口日志（可整体删，见 `docs/code-map.md` 第 9 节）。
     **（2026-10-19 起 SD 默认打开、结构体对齐修正后布局有变，最新数字见下一条第 5 项。）**
5. **多音色（每通道一个音色）+ SD 卡启用（2026-10-19，均已上板验证）**：
   - 多音色：`Engine::chan_patch[16]`/`chan_idx[16]`，Program Change 只切本通道（标准 GM 行为），
     新建 `ApplyPatchIndexToChannel()`；`StartVoice()`/`AllocateVoice()`/`VoiceNoteOn/Off()`/`EnsureFmVoice()`
     全部带 `ch` 参数；`ControlChange()` 的 patch 类 CC（5/72/73/76/77/79）写本通道并推给该通道在响的声部；
     声部侧是 `Voice::patch_` 快照（`Synth/src/core/voice.h`，`NoteOn` 时 memcpy、`Render` 里只读自己的）。
     对外 API：`synth_set_channel_patch(ch, idx)` / `synth_get_channel_patch(ch)`（越界 0xFFFF）。
     已知限制：FM 引擎一次只能载入一条 DX7 音色（两个不同 FM 音色的通道会来回换载）、ROMpler 乐器全局一个、
     效果链全局一条（参数跟随"主音色"`e.patch`，即最后一次切换的音色）。
   - SD 卡：`SYNTH_PORT_SD_ENABLE = 1`；**插卡才挂载**（PD4 低 = 卡在座，没卡 `sd_mount = -2` 且不置故障位，
     因为没卡时 `hsd1` 未初始化、直接 `f_mount` 会走到 `HAL_SD_Init` 上）。
     文件约定 `0:/synth/NNN.syn`（文本音色或 DX7 `.syx`）与 `0:/synth/NNN.ins`（采样乐器）：
     Program Change 后主循环的 `synth_port_patch_service()` 发现音色号变化就尝试从 SD 覆盖内置音色。
   - 为塞进 128 KB flash：FatFs 关掉 `_USE_MKFS`/`_USE_FASTSEEK`/`_USE_STRFUNC` 并开 `_FS_TINY`
     （`FATFS/Target/ffconf.h`）；`FatFs` 库、`STM32_Drivers` 里的 `sdmmc.c`、TinyUSB 六个源文件用 `-Os`
     编译（`CMakeLists.txt`）。`FATFS/Target/sd_diskio.c` 的 `SD_read/SD_write` 前后各加一次
     `SCB_CleanInvalidateDCache()`（SDMMC 用 IDMA 直写 AXI SRAM，D-Cache 感知不到；CubeMX 重生成会覆盖）。
   - 实测（SD 打开）：**FLASH 125,168 B (95.50%)**、AXI RAM 409,024 B (78.02%)、DTCMRAM 46,336 B (35.35%)。
     ⚠ flash 只剩约 6 KB —— 再加功能之前先删诊断代码（8~11 KB，见 `docs/code-map.md` 第 9 节）。
   - 上板验证：ch0 发 PC 5、ch1 发 PC 8 后，`g_app_status.chan_patch[0] = 0x00000085`
     （`tools/status.ps1` 打印 `ch0=PC5(Square Keys)   ch1=PC8(PWM Strings)`）；`fault = 0`、
     `audio_underruns = 0`、`cpu_permille ≈ 142‰`、`sd_mount = -2`（当时没插卡）。
    - **USB 板级初始化补了一处必需项**：`Synth/port/platform_stm32.c` 的 `usb_hw_init()` 现在第一句是
      `HAL_PWREx_EnableUSBVoltageDetector()`。H7 的 FS PHY 由 VDD33USB 供电，电平检测器不开时 DP 上拉
      不生效 → 主机侧完全看不到设备（连"未知 USB 设备"都不出现）。CubeMX 为同一块板生成的
      `HAL_PCD_MspInit()` 第一句就是它；TinyUSB 的 dwc2 驱动不管 PWR。
    - **USB 诊断**：`g_app_status` 现在是 **32 个 u32** —— 除 `usb_mounted` = `tud_mounted()`、
      `midi_rx_bytes` 之外，还有 `boot_count` / `reset_flags`（判断**复位循环**；⚠ 烧写和按复位键都会让
      `boot_count` +1，要「不烧写不复位连读两次看是否增长」）、`usb_mount_count` / `usb_umount_count`
      （枚举/掉线次数）、USB 寄存器快照（`usb_irq_count` / `usb_dctl` / `usb_dsts` / `usb_gotgctl` /
      `usb_gintsts` / `usb_gccfg` / `usb_gusbcfg` / `usb_dcfg` / `usb_sel` / `usb_clk_rc`；**仅在未枚举时刷新**）、
      `loop_count`，以及存活探针 `systick_ticks` / `main_stage` / `usb_isr_depth` / `audio_isr_depth`。
      ⚠ `usb_ck_hz` 不能用 `HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_USB)` —— H7 的 HAL 里
      **没有 USB 分支**（`stm32h7xx_hal_rcc_ex.c:1881` 起），恒返回 0；现已改为按 mux + PLL1Q 自算。
    - **PE3 心跳灯**（`App_LedTask`，不接调试器也能看状态）：100 ms 快闪 = 有故障位；
      1 Hz 慢闪 = 主循环在跑但 USB 未枚举；每 2 秒一次短脉冲 = 一切正常。
    - **一条命令看状态**：`tools/status.ps1`（VS Code 任务"读取板子状态"）——自己从 ELF 里查
      `g_app_status` 地址（重编译不会失效），openocd 读回后解码成表，附故障位、复位原因和结论。
      排查顺序见 `docs/phase2-review.md` 第 7 节第 6 条。
    - **HardFault 现场记录**（2026-10-18 晚）：`HardFault_Handler` 先调 `App_HardFaultRecord()`
      （`Core/Src/main.c`，`naked` + 内联汇编、不压栈），把现场写进 `g_fault`
      （**20 个 u32**，在 `.ram_section`，软复位不丢；本次构建地址 `0x2400BF00`）：
      `count/CFSR/HFSR/MMFAR/BFAR/ICSR/EXC_RETURN/MSP/PSP` + `r0-r3/r12/LR/PC/xPSR`；
      存完**不复位**，PE3 改为**三短一长**循环闪。背景：调试器里单步不炸、放它自己全速跑必炸
      ⇒ 故障几乎肯定在中断上下文（首选嫌疑：音频 DMA1_Stream0，优先级 0）。
      `tools/status.ps1` 已能解码这一段（CFSR 逐位、犯规地址、出事时的异常号、PC/LR 源码行）。
   - **构建/下载脚本**：`tools/build.ps1`（`-BuildType/-Generator/-BuildDir/-Clean/-Flash/-NoSize`）、
     `tools/flash.ps1`（OpenOCD + CMSIS-DAP；不指定 `-Elf` 时**优先** `build\<BuildType>\STM32synthesizer.elf`，
     没有再挑最新的，避免误烧旧构建目录里遗留的 ELF；`-DryRun` 只打印命令）、
     `.vscode/tasks.json`（七个任务）、`CMakePresets.json`（四组预设，**务必带 toolchainFile**）。
     ⚠ 三个 .ps1 必须保持 **UTF-8 BOM**：PowerShell 5.1 会把无 BOM 的 UTF-8 当 GBK 读，中文注释乱码后
     整个脚本报 `Unexpected token`；而 `edit` 工具会**悄悄抹掉 BOM**，改完必须补回。
5. **仍需做**：
   - ⚠ 启用 SD（`Synth/port/synth_port_config.h` 的 `SYNTH_PORT_SD_ENABLE` 置 1）之前，先解决
     SDMMC 内部 IDMA 与 D-Cache 的一致性（FATFS 对象/采样池目前是可缓存的 AXI SRAM）。
     SD 路径本身已能编译+链接（顺手修掉两个藏在开关后面的 bug：`patch_storage_sd.c` 少了
     `synth_port.h` 导致 `synth_port_instrument_load` 隐式声明、`sample_storage_sd.c` 还在用旧宏名
     `SYNTH_SAMPLE_POOL_BYTES`），采样池默认已从 256 KB 降到 128 KB（256 KB 时 AXI 99.88%）。
   - 真机验证：DMA/Cache 一致性、CPU 负载与 xrun 计数、QSPI 采样读取（dummy cycles 若错位改 6）。
   - 采样增强：速度分层 + "最近根音"音区选择、WAV 8/24bit 支持、采样库清单扫描（`/synth/*.ins` 列表）。

## 6. 踩过的坑（别重复踩）
- msfa：必须先调 `Exp2/Tanh/Sin/Freqlut/Env/PitchEnv/Porta/Lfo` 的 init，否则空表指针 → 0xC0000005；
  `ctrls_.core` 必须指向 `EngineMsfa` 实例；类名是 `Freqlut`。
- 弱符号在 MinGW/PE 下不工作 → 引擎用 `synth_platform_hooks_t` 钩子表。
- 测试崩溃时 stdout 缓冲会丢日志 → main 里 `setvbuf(stdout, nullptr, _IONBF, 0)`。
- 测量缓冲隔点写入 = 插零 → 频率读数减半；std::vector 越界 → STATUS_HEAP_CORRUPTION。
- C++ 成员函数名会遮蔽同名嵌套类型（`Held()` vs `struct Held`）。
- git 直连走不可用代理 → `git -c http.proxy= -c https.proxy= clone`。
- **compress 工具在旧会话里曾永久失效**（折叠基线代际错位，见 `acp_status` 的 FOLD BASE GENERATION MISMATCH）；
  解决办法是开新会话，而本文件就是为了让新会话能无缝接手。（2026-10-18 的新会话里 compress 已恢复正常。）
- **TinyUSB 本地补丁（升级库时必须重打）**：`tu_edpt_packet_size()`
  （`Synth/third_party/tinyusb/src/common/tusb_types.h`）改成 `volatile` 逐字节读 `wMaxPacketSize`。
  USB-MIDI 描述符的端点落在奇数地址，`ldrh` 在 Cortex-M7 上非对齐访问 → UsageFault → HardFault，
  正好死在 `SET_CONFIGURATION`，表现是 Windows 代码 10、固件 `usb_mounted = 0`。详见
  `docs/phase2-review.md` §8。
- **`arm-none-eabi-nm -S` 的分列**是 `地址 大小 类型 名字`：取地址用 `(\$line -split '\s+')[0]`
  （用 `[1]` 会读到大小 → dump 到垃圾地址）。
- **openocd 的 `mdw`/`echo` 输出经 PowerShell 捕获会丢**（只有 stderr 的 Info/Error 能看到）：
  读内存一律 `dump_image {文件} 0x地址 长度` 再用 `[BitConverter]::ToUInt32` 解。
- **2026-10-19 清理**：删掉死代码 `synth_port_hard_fault_hook()`（只有定义、全工程无引用）、
  `dbg_hex()` / `dbg_take_dropped()`（无人调用）；`SYNTH_PORT_MIDI_TRACE` 默认改 **0**（每包一行太吵）；
  新增 `SYNTH_PORT_FAULT_RESET`（**0 = 默认**：记录现场 + 三短一长闪灯、不复位；1 = 记录后软复位以便自动恢复）。
  HardFault 现场、存活探针、心跳灯**保留**（成本几十字节，是排查"卡住/复位"的唯一手段）。
- **`sizeof` 不是 4 的倍数的结构体，禁止整体赋值/按值传参**（2026-10-19，切音色必崩的根因）：
  `synth_patch_t` 全是 `uint8_t` ⇒ `sizeof = 53`、对齐 1，`e.patch = e.bank[5]` 的源地址是
  `bank+265`（奇数）→ GCC 展开成 `ldr/str` 整字拷贝 → Cortex-M7 非对齐字访问 → UNALIGNED
  HardFault（上位机一点「切换音色」就卡死、PE3 三短一长）。修法：`Synth/include/synth/patch.h`
  加 `__attribute__((aligned(4)))`（`sizeof` 自动补到 56）+ `engine.cpp:106/:305` 改 `memcpy`。
  判据：全 `uint8_t` 的结构体先看 `sizeof % 4`。详见 `docs/phase2-review.md` §10。
- **HardFault 记录器取 `EXC_RETURN` 不能读 `LR`**：Handler 里 `bl` 已经把 LR 换成了返回地址，
  真正的 `EXC_RETURN` 在 `[MSP+4]`（Handler 序言 `push {r3, lr}` 存下来的），异常帧在 `[MSP+8]`；
  找帧时也不要要求 IPSR≠0（线程模式出事时 IPSR 就是 0）。详见 `phase2-review.md` §8 末尾。
- **MIDI 解析**：系统公共消息 `0xF1`(1)/`0xF2`(2)/`0xF3`(1)/`0xF6`(0) 不能当 2 个数据字节，
  且不建立 running status（见 `Synth/src/midi/midi_parser.h`）。上位机 `D:\Tools\midi_ctl` 的
  「切换音色」发的是标准 Program Change（`0xC0|ch`，program = 滑块值−1，0..127），
  只有 0..14 命中本机音色库，其余按设计忽略（引擎里 `unknown_cc`/`req_patch_index` 越界保护）。
- **`tools\build.ps1` 别让它在"有 Ninja 的机器"上自动挑 Ninja**：本机（沙箱环境）里 `cmake -G Ninja`
  会卡死在 `-- Detecting C compiler ABI info`，构建目录里只剩 `CMakeFiles\CMakeScratch`。
  验证时统一加 `-Generator 'MinGW Makefiles'`；被中断的构建会留下 cmake/ninja 僵尸进程
  （`Get-Process cmake,ninja | Stop-Process -Force` 后再重配）。
- **`edit` 工具会抹掉 `.ps1` 的 UTF-8 BOM**，症状是脚本报 `Unexpected token`（还伴随中文提示乱码），
  很像"脚本逻辑被改坏了"。改完立刻补：
  `[System.IO.File]::WriteAllText($p, $t, (New-Object System.Text.UTF8Encoding($true)))`。
