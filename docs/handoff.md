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
   - 实测（RelWithDebInfo，SD 关闭）：FLASH 92,168 B (70.3%)、DTCM 46,336 B (35.4%)、AXI 247,548 B (47.2%)；
     `SYNTH_PORT_SD_ENABLE=1` 时 FLASH 109,532 B (83.6%)、AXI 392,576 B (74.9%)。
    - **USB 板级初始化补了一处必需项**：`Synth/port/platform_stm32.c` 的 `usb_hw_init()` 现在第一句是
      `HAL_PWREx_EnableUSBVoltageDetector()`。H7 的 FS PHY 由 VDD33USB 供电，电平检测器不开时 DP 上拉
      不生效 → 主机侧完全看不到设备（连"未知 USB 设备"都不出现）。CubeMX 为同一块板生成的
      `HAL_PCD_MspInit()` 第一句就是它；TinyUSB 的 dwc2 驱动不管 PWR。
    - **USB 诊断**：`g_app_status` 增到 **28 个 u32** —— 除 `usb_mounted` = `tud_mounted()`、
      `midi_rx_bytes` 之外，还有 `boot_count` / `reset_flags`（判断**复位循环**：HardFault 钩子是
      `NVIC_SystemReset()`，肉眼只看得到"枚举失败"；⚠ 烧写和按复位键都会让 `boot_count` +1，
      要「不烧写不复位连读两次看是否增长」）、`usb_mount_count` / `usb_umount_count`（枚举/掉线次数），
      以及第三轮加的 USB 寄存器快照 `usb_irq_count`（OTG_FS 中断次数，恒为 0 = 核没中断）、
      `usb_dctl` / `usb_dsts` / `usb_gotgctl` / `usb_gintsts` / `usb_gccfg` / `usb_gusbcfg` / `usb_dcfg`、
      `usb_sel` / `usb_clk_rc`、`loop_count`（不涨 = 主循环卡死）。主循环里刷新。
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
     `tools/flash.ps1`（OpenOCD + CMSIS-DAP，自动挑最新的 `STM32synthesizer.elf`，`-DryRun` 只打印命令）、
     `.vscode/tasks.json`（六个任务）、`CMakePresets.json`（四组预设，**务必带 toolchainFile**）。
     ⚠ 两个 .ps1 必须保持 **UTF-8 BOM**：PowerShell 5.1 会把无 BOM 的 UTF-8 当 GBK 读，中文注释乱码后
     整个脚本报 `Unexpected token`。
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
