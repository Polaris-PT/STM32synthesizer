# 阶段 2 检查报告：CubeMX 生成工程 + main.c 重构

日期：2026-10-18　范围：CubeMX 6.18.1 / FW_H7 V1.13.0 / Toolchain=CMake 生成的工程 + `Synth/port` 对接层
（配置目标见 [cubemx-checklist.md](cubemx-checklist.md)，硬件约束见 [handoff.md](handoff.md)）

## 1. 结论（TL;DR）

- `Core/Src/main.c` **已重构**（503 行，全部写在 `USER CODE` 区内，CubeMX 重新生成不会覆盖）：
  SAI3 内核时钟改到 PLL2（24.576 MHz）、协议改 32bit/2slot、MCLK 输出使能、`MCKDIV` 重算、
  GPIO 提速、**SAI3_B 的 DMA 软件兜底**、MPU 修正（QSPI 映射区可读 + QUADSPI 寄存器段可访问）、
  USB 时钟 48.000 MHz、应用启动序列与故障位图。
- **工程能完整编译链接**（实测，非推测）：72 个源文件全部通过，`STM32synthesizer.elf` 链接成功，`-Wall` 无警告。
  默认构建（RelWithDebInfo，`-O2 -g`）：FLASH **84,152 B / 128 KB (64.2%)**、
  DTCMRAM **94,960 B / 128 KB (72.5%)**、AXI SRAM（`.ram_section`）**194 KB / 512 KB (37.9%)**；
  `-DCMAKE_BUILD_TYPE=Release`（`-Os`）下 FLASH 降到 78,448 B (59.9%)。
- 生成代码里有 **11 处真实缺陷**（第 3 节），其中 2 处会直接 HardFault/MemManage、3 处会导致静音或走音，
  已在 `main.c` / 端口层 / 链接脚本 / CMake 里修好；**CubeMX 侧还有 5 项需要你手动改**（第 4 节），
  改完以后 `main.c` 里的软件兜底会自动让位，不会打架。

## 2. 时钟与音频链路（现在的实际状态）

| 项 | 值 | 备注 |
|---|---|---|
| HSE | 25 MHz | 板上无源/有源晶振，RCC 里走 HSE 旁路 |
| PLL1 | M=5, N=192, P=2, **Q=20**, R=2 | SYSCLK 480 MHz（VOS0 + FLASH_LATENCY_4）；**PLL1Q = 48.000 MHz → USB** |
| PLL2 | M=5, N=108, P=22, FRACN=1101 | VCO 540.672 MHz → **PLL2P = 24.57599987 MHz（−0.13 Hz ≈ −5 ppb）→ SAI23 内核** |
| SAI3 MCKDIV | 2 | MCK = 24.576/2 = **12.288 MHz**（256×fs）→ Fs = 48.000 kHz |
| HCLK / APB | 240 MHz / 全部 DIV2 | |
| SAI3 | Block B，Master TX，I2S Philips，**32bit × 2 slots**，MckOutput=ENABLE | PD8=SCK, PD9=SD, PD10=FS, PD14=MCLK，AF6，VERY_HIGH |
| 音频 DMA | DMA1 Stream0，DMAMUX 请求 114（SAI3_B），循环、WORD、Very High、NVIC 优先级 0 | CubeMX 侧没配 → `main.c` 兜底 |
| 音频缓冲 | `g_audio_buf`（8 KB，2×128 帧×2slot×32bit）在 AXI SRAM `.ram_section` | 每次渲染后 `SCB_CleanDCache_by_Addr()` |
| USB | OTG_FS Device（TinyUSB dwc2），PA11/PA12 AF10，NVIC 优先级 5，VID 0xCafe / PID 0x4020 | `platform_stm32.c` 里做引脚+时钟+NVIC |

**为什么 SAI 必须放在 `MX_SAI3_Init()` 之后再改**：HAL 是在 `HAL_SAI_MspInit()` **之后**按当时的内核时钟
算 `MCKDIV` 的（`Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_sai.c:598`），而生成的 `MspInit()` 会把
`SAI23SEL` 写回 PLL1Q。所以顺序必须是：`MX_SAI3_Init()` → 改 mux 到 PLL2P → 按新时钟重算 `MCKDIV`。
`main.c` 里的 `App_ClockConfig_Audio()` / `App_ApplySaiMckdiv()` 就是这两步，并对结果做了断言
（`g_app_status.audio_ck_hz` 必须 == 24,576,000，否则 `fault |= APP_FAULT_SAI_CLK`）。

## 3. 生成工程里发现的问题（及处理状态）

| # | 问题 | 后果 | 处理 |
|---|---|---|---|
| 1 | 没有任何 `HAL_RCCEx_PeriphCLKConfig`：SAI23 内核时钟 = PLL1Q = 137.14 MHz，`.ioc` 里 SAI3 `RealAudioFreq = 53.571 kHz / Error 11.6%` | 采样率错 +11.6%，全音域走音 | ✅ `main.c` 改 PLL2P 24.576 MHz |
| 2 | `MX_SAI3_Init()` 用 `SAI_PROTOCOL_DATASIZE_16BIT` | 32bit 立体声数据被当 16bit，左右声道/字长错 | ✅ `main.c` 重调 `HAL_SAI_InitProtocol(..., 32BIT, 2)` |
| 3 | `HAL_SAI_MspInit()` 里没有 DMA（无 `__HAL_LINKDMA`、无 DMA1 时钟、无 NVIC）→ `hsai_BlockB3.hdmatx == NULL` | `HAL_SAI_Transmit_DMA()` 直接失败：**静音** | ✅ `main.c` 的 `App_SaiDmaFallback()` + `DMA1_Stream0_IRQHandler()`（`hdmatx != NULL` 时自动让位） |
| 4 | MPU Region0 = 4 GB NO_ACCESS，`SubRegionDisable = 0x87` → `0x90000000` 段被禁（QSPI 映射一读就 MemManage）；`0x80000000`/`0xA0000000` 段也被禁 | QSPI 采样区不可读；QUADSPI 寄存器（`0xA0001000`，在 `0xA0000000` 段内）第一次写就 fault | ✅ `SubRegionDisable = 0xB7` 放开两段，并加 Region1 = `0x90000000` 8 MB 非缓存只读 |
| 5 | `.ioc` 的 QSPIFLASH 区域（`MMTAppReg7`，8 MB @ `0x90000000`）**没有生成任何 MPU 代码** | 同上 | ✅ 手写 Region1 |
| 6 | 没有 PLL2、PLL1Q=7 | USB 拿不到 48.000 MHz（137.14 MHz），TinyUSB 枚举失败 | ✅ `PLL1Q=20` + `platform_stm32.c` 里的 USB 时钟源配置（已在端口层，本次核对通过） |
| 7 | SAI3 四个引脚 `GPIO_SPEED_FREQ_LOW` | 12.288 MHz 方波边沿过缓，MCLK/SCK 质量差 | ✅ `App_SaiGpioSpeed()` 改 VERY_HIGH |
| 8 | `STM32H750xx_FLASH.ld` 没有 `.ram_section` 输出段 | `Synth/port` 里所有 `.ram_section` 变量（音频缓冲、FX 池、采样池）变成孤立段，链接期可能被丢/报错 | ✅ 加 `.ram_section (NOLOAD) >RAM`（AXI SRAM） |
| 9 | 链接脚本 `_Min_Stack_Size = 0x400`（1 KB） | 引擎 + TinyUSB + printf 的余量太薄 | ✅ 提到 0x2000（8 KB）；实测最深函数帧 < 300 B，改完 DTCM 仍余 ~33 KB |
| 10 | 根 `CMakeLists.txt` 只有 C/ASM，没有引擎源、没有 TinyUSB、默认 Debug | 引擎（C++）根本编不进来 | ✅ 加 CXX/gnu++17、`Synth/src`+`msfa`+`port`+TinyUSB 源、`-O2`、默认 RelWithDebInfo |
| 11 | `Synth/port/synth_port_config.h:9` 声明的是 `hsai3`，生成句柄叫 `hsai_BlockB3` | 链接期 `undefined reference to hsai3` | ✅ 改成 `hsai_BlockB3` |

其它已核对但**不需要改**的：`synth_port_init()` 里的 USB 引脚/时钟/NVIC（`platform_stm32.c:20-42`）、
`audio_out_sai.c` 的 D-Cache 写回、`stm32h7xx_it.c` 里 `SysTick_Handler → synth_tick_1ms()` 与
`OTG_FS_IRQHandler → tud_int_handler(0)`（都在 `USER CODE` 区内，重新生成会保留）、
SAI 半传输/完成回调只在 `audio_out_sai.c` 实现（不要在别处重复实现）。

## 4. CubeMX 侧需要你手改的（按优先级）

1. **RCC**：`PLL1 Q = 20`（USB 48.000 MHz）；`PLL2 = M5 / N78 / FRACN5269 / P16`；
   **SAI23 Clock Mux = PLL2**；USB Clock Mux = PLL1Q。
   > ⚠ 老的 `docs/cubemx-checklist.md` 里 PLL2 写的是 `N=39 / FRACN=21076`——`PLL2FRACN` 只有 13 bit
   > （0..8191），21076 是**无效值**，已在文档里改正。
2. **SAI3**：Data Size / Slot Size = **32 bits**，Number of Slots = **2**，
   Master Clock Output **使能**，Audio Frequency 48 kHz —— 参数页 `Real = 48.000 kHz / Error 0.00%` 才算对。
3. **SAI3 的 DMA**：Add → `SAI3_B`（不是 SAI3_A），DMA1 Stream0，Circular，Word，Very High；
   NVIC 里 DMA/SAI3 中断优先级 **0**（必须高于 OTG_FS 的 5）。
4. **SAI3 GPIO**：Speed = Very High（代码已兜，改了更干净）。
5. **QUADSPI**：现在的值是错的——`ClockPrescaler = 255`（≈938 kHz）、`FlashSize = 1`（=4 字节）、
   `SampleShifting = NONE`。要做 QSPI 采样（阶段 2/3）需：`ClockPrescaler ≈ 4`（48 MHz）、
   `FlashSize = 23`（8 MB → 寄存器值 23）、`SampleShifting = HalfCycle`；**并且代码里还得补**
   flash 复位/ID/置 QE + `HAL_QSPI_MemoryMapped()`（`0xEB` 四线快读）——`Synth/port/sample_storage_qspi.c`
   只做零拷贝寻址，假定外部已经进了内存映射模式。目前**没有**任何代码做这件事。
6. （阶段 1 剩余）**SDMMC1 + FATFS**：PC8..PC11 = D0..D3、PC12 = CK、PD2 = CMD、PD4 = 卡检测。
   配好后把 `Synth/port/synth_port_config.h` 的 `SYNTH_PORT_SD_ENABLE` 改成 1
   （现在是 0：`sample_storage_sd.c` / `patch_storage_sd.c` 退化成空实现，缺 `ff.h` 才不会编译失败），
   注意 SDMMC 中断优先级要低于音频 DMA。
7. **USB**：只需要在 Clock 页把 USB 时钟源选上（或干脆不动，`platform_stm32.c` 已兜底）；
   **不要**启用 ST 的 USB_DEVICE 中间件 / `stm32h7xx_hal_pcd.c`（TinyUSB 自带 dwc2，省 flash）。

## 5. 改动的文件

应用层（git 里可见，`git diff --stat` = 6 files, +111/−19）：

| 文件 | 改动 |
|---|---|
| `Synth/port/synth_port_config.h` | 句柄名 `hsai_BlockB3`；新增 `SYNTH_PORT_SD_ENABLE`（默认 0）与 `SYNTH_PORT_SAMPLE_POOL_BYTES`（512 KB → 128 KB，256 KB 时 AXI 99.88% 装不下） |
| `Synth/port/audio_out_sai.c` | 渲染后 `SCB_CleanDCache_by_Addr()`（+18 行） |
| `Synth/port/platform_stm32.c` | USB 引脚 PA11/PA12 AF10 + OTG_FS 时钟 + NVIC |
| `Synth/port/usb_midi_tinyusb.c` | 注释里的 `/*` 造成 `-Wcomment` 警告（已清） |
| `Synth/port/sample_storage_sd.c` / `patch_storage_sd.c` | 用 `SYNTH_PORT_SD_ENABLE` 包住 FatFs 依赖 |

生成侧（CubeMX 产物，未纳入 git）：`Core/Src/main.c`（重构，503 行）、`STM32H750xx_FLASH.ld`（`.ram_section` + 8 KB 栈）、
`CMakeLists.txt`（C++/引擎/TinyUSB/RelWithDebInfo）。`Core/Src/stm32h7xx_it.c` 无需改动。

## 6. 实测构建（本机已验证）

```powershell
cmake -S . -B build-mingw -G "MinGW Makefiles" `
      -DCMAKE_MAKE_PROGRAM=<mingw32-make> `
      -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake
cmake --build build-mingw -j 8          # → build-mingw/STM32synthesizer.elf
```

- 结果：**72/72 源文件编译通过，链接成功**，无 error、无警告。
- 占用（默认 RelWithDebInfo）：FLASH **84,152 B / 128 KB (64.2%)**、DTCMRAM **94,960 B / 128 KB (72.5%)**、
  AXI SRAM 194 KB / 512 KB (37.9%)、RAM_D2/D3/ITCM 0；`Release` 下 FLASH 78,448 B (59.9%)。
- ⚠ 本机（这个沙箱里）`cmake -G Ninja` 会在 `-- Detecting C compiler ABI info` 卡死，用最小工程也能复现，
  **与本工程无关**（cmake→ninja 子进程链问题）。你自己的机器上用 Ninja 或 IDE 正常配置即可；
  在这台机器上就用上面的 MinGW Makefiles 命令。

## 7. 上板验收步骤（还没做，需要硬件）

1. 烧写：`openocd -f interface/cmsis-dap.cfg -f target/stm32h7x.cfg -c "program build-mingw/STM32synthesizer.elf verify reset exit"`
2. 调试器 watch `g_app_status`：`fault` 应为 **0**；`sysclk_hz=480000000`、`hclk_hz=240000000`、
   `usb_ck_hz=48000000`、`audio_ck_hz=24576000`、`sai_mckdiv=2`。
3. 频率计/示波器：PD14 (MCLK) = **12.288 MHz**、PD10 (FS) = **48.000 kHz**（±100 ppm 内），PD8/PD9 有数据。
4. USB 插电脑：应枚举成 MIDI 设备（VID `0xCafe` / PID `0x4020`）。⚠ Windows 里它显示成
   **"USB 音频设备"**（在"声音、视频和游戏控制器"下），**不会写"MIDI"**，别在"MIDI 设备"里找。
   发 Note On 应出声；`g_synth_audio_xruns` 应保持 0（有增长说明 DMA 来不及，先看 CPU 负载
   `synth_cpu_load_permille()`）。
5. 状态读法（推荐直接用脚本：地址每次重编译都可能变，脚本会自己从 ELF 里查）：

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File tools\status.ps1    # 或 VS Code 任务"读取板子状态"
   ```

   它读回 `g_app_status`（**32 个 u32**）+ `g_synth_audio_xruns` + `g_synth_midi_rx_bytes`，解码成表，
   并打印故障位、复位原因（`RCC->RSR`）、**USB 寄存器判读**、**存活探针**和一句结论。
   ⚠ 不要手敲固定地址的 `mdw`：重编译后地址会变（本次 `g_app_status` 在 `0x240000CC`）。
   要用符号就 `arm-none-eabi-nm -S build\RelWithDebInfo\STM32synthesizer.elf | Select-String g_app_status`。
   （脚本读的时候 CPU 会短暂 halt，主机可能因此认为 USB 掉线一次 —— 所以判断
   `usb_umount_count` 要先读一次作基线，再看后续是否增长。）

   | 偏移 | 字段 | 期望值 |
   |---|---|---|
   | +0x00 / +0x04 / +0x08 | sysclk_hz / hclk_hz / usb_ck_hz | 480000000 / 240000000 / 48000000 |
   | +0x0C / +0x10 / +0x14 | audio_ck_hz / sai_mckdiv / qspi_id | 24576000 / 2 / 0xEF4017 |
   | +0x18 | fault | 0（位定义见 `Core/Src/main.c` 顶部） |
   | +0x1C / +0x20 / +0x24 / +0x28 | synth_init / usb_init / sd_mount / audio_start | 0 / 0 / -1（未启用 SD）/ 0 |
   | +0x2C | usb_mounted | 插着 USB 时 = 1（`tud_mounted()`） |
   | +0x30 | midi_rx_bytes | 弹一下琴键就涨 |
   | +0x34 / +0x38 | boot_count / reset_flags | 见第 6 节第 8 条（别只看数字） |
   | +0x3C / +0x40 | usb_mount_count / usb_umount_count | 枚举/掉线次数 |
   | +0x44 / +0x48 / +0x4C | usb_irq_count / usb_sel / usb_clk_rc | >0（插着线）/ 1 / 0 |
   | +0x50 / +0x54 / +0x58 / +0x5C | usb_gotgctl / usb_gintsts / usb_dctl / usb_dsts | 见第 6 节第 9 条 |
   | +0x60 / +0x64 / +0x68 / +0x6C | usb_gccfg / usb_gusbcfg / usb_dcfg / loop_count | PWRDWN=1 / PHYSEL=1 / DSPD=3 / 持续涨 |
   | +0x70 / +0x74 / +0x78 / +0x7C | systick_ticks / main_stage / usb_isr_depth / audio_isr_depth | 1 kHz 持续涨 / 1=usb_task 2=patch_service 3=synth_service 0=其它 / 正常时 0 / 正常时 0 |

   **PE3 心跳灯**（`App_LedTask`，不接调试器也能看）：100 ms 快闪 = 有故障位；
   1 Hz 慢闪 = 主循环在跑但 USB 未枚举；**每 2 秒一次短脉冲 = 一切正常**。

6. **USB 插上电脑没反应时的排查顺序**（2026-10-18 实际遇到）：
   1. 先看 `fault`：bit7 置位 = 卡在 `Error_Handler()`（USB 根本没跑到）；bit4 = `tusb_init()` 失败。
      `fault = 0` 而 `usb_mounted = 0` → 固件在跑、主机没完成枚举，继续下一步。
   2. **硬件自检（先做这个，能一刀切开固件/硬件）**：按住 BOOT0 再按 RESET → Windows 应出现
      "STM32 Bootloader"（DFU）。**不出现 = 线/口/连接器问题**（必须是数据线、直插主板 USB 口、别过 hub），
      与固件无关。
   3. **已修的真实缺陷**：`Synth/port/platform_stm32.c` 的 `usb_hw_init()` 原来漏了
      `HAL_PWREx_EnableUSBVoltageDetector()`。H7 的 FS PHY 由 VDD33USB 供电，内部 USB 稳压器使能时
      电平检测器也必须打开（HAL 原文：the VDD33USB supply level detector **shall** be enabled，
      `stm32h7xx_hal_pwr_ex.c:1598-1602`）；不开则 DP 上拉不生效，主机侧连"未知 USB 设备"都不会出现。
      CubeMX 为同一块板生成的 `HAL_PCD_MspInit()` 第一句就是它
      （`资料\MiniSTM32H7xx-master\SDK\HAL\STM32H750\06-SPIFlash_Test\USB_DEVICE\Target\usbd_conf.c:77`）。
      TinyUSB 的 dwc2 驱动不管 PWR：`dwc2_stm32.h` 的 `dwc2_phy_init()` 只设 `GCCFG.PWRDWN` +
      关 ULPI 睡眠时钟，`dwc2_clock_init()` 是空函数。
   4. **已修的第二处缺陷（"该设备无法启动。(代码 10)"）**：MIDI 描述符集里有 **2 个接口**
      （0 = AudioControl、1 = MIDIStreaming，`TUD_MIDI_DESCRIPTOR(n,…)` 内部用 n+1），而
      `ITF_NUM_TOTAL` 曾误写成 **1** → `bNumInterfaces = 1` 与描述符内容自相矛盾：Windows 读得出
      设备名（枚举前半段成功、设备管理器里能看到 "H750 PCM5102A Synth"），但 `usbmidi.sys` 启动失败，
      报"该设备无法启动。(代码 10) I/O 设备的设置不正确或驱动程序的配置参数不正确"，且**不创建 MIDI
      端口**（DAW 里看不到设备）。现在 `Synth/port/usb_midi_tinyusb.c` 里是
      `ITF_NUM_MIDI 0 / ITF_NUM_MIDI_STREAMING 1 / ITF_NUM_TOTAL 2`；从编译产物里 dump 出来的配置
      描述符首部是 `09 02 65 00 02 01 00 80 32`（wTotalLength=101、**bNumInterfaces=2**）。
      ⚠ Windows 会把这个"启动失败"状态缓存住（设备实例 ID 不变：`USB\VID_CAFE&PID_4020\0001`），
      所以改完要**先在设备管理器里卸载设备（勾上"删除此设备的驱动程序软件"）再插**。
   5. Windows 侧直接查：`Get-PnpDevice | Where-Object InstanceId -like '*VID_CAFE*'`（或 `usbview.exe`）。
      出现"未知 USB 设备（设备描述符请求失败）"= 信号/描述符问题，把原话报出来。
   6. 核过、确认**不是**问题的点：TinyUSB 的 rhport0 = **USB2_OTG_FS**(`0x40080000`) / `OTG_FS_IRQn`(101)
      （`dwc2_stm32.h:129-137` + `stm32h750xx.h:2778`），与 `__HAL_RCC_USB_OTG_FS_CLK_ENABLE()`
      （legacy 别名 = `USB2_OTG_FS`，`stm32_hal_legacy.h:3647`）、`it.c` 的
      `OTG_FS_IRQHandler → tud_int_handler(0)` 完全一致；PA11/PA12 的 AF10 在 H750 上就是 OTG2_FS
      （宏名 `GPIO_AF10_OTG1_FS` 有误导性，值 0x0A 是对的）；D-Cache 也无风险 ——
      `CFG_TUD_DWC2_DMA_ENABLE_DEFAULT = 0`（`tusb_option.h:339`）→ TinyUSB 走 slave/FIFO 模式，
      不用总线 DMA，没有 cache 一致性问题。
   7. **实测到的两个"假警报"（2026-10-18 晚，别被它们带偏）**：
      - `usb_ck_hz` 读到 **0** ≠ 时钟坏了。H7 这版 HAL 的 `HAL_RCCEx_GetPeriphCLKFreq()` 的 if 链里
        **根本没有 USB 分支**（`stm32h7xx_hal_rcc_ex.c:1881` 起；配置侧的 USB 分支在 `:1368`），
        任何情况下都返回 0。现在 `main.c` 的 `App_UsbKernelClockHz()` 按 `__HAL_RCC_GET_USB_SOURCE()`
        + `HAL_RCCEx_GetPLL1ClockFreq()` 自己算，并把原始 `USBSEL` 记进 `usb_sel`。
      - `boot_count > 1` ≠ 复位循环。烧写（openocd 的 `program … reset exit` → `SFTRSTF`）和按复位键
        （`PINRSTF`）都会让它 +1，而且 `RCC->RSR` 的多种复位标志可能一起置位。**正确判据**：不烧写、
        不按复位，连读两次，只有两次之间增长才是循环（脚本已改成这样措辞）。
   8. **第三轮新增的 USB 探针**（都在 `g_app_status`，主循环每圈刷新，用于区分
      「核没产生中断」/「中断了但没谈成」/「PHY 或时钟没起来」）：
      `usb_irq_count` = OTG_FS 中断次数（插着线恒为 0 → DWC2 核根本没中断：时钟/PHY/上拉/中断使能）；
      `usb_dctl.bit0 RWUSIG` = 1 → 软断开（DP 上拉没接上，主机永远看不到设备）；
      `usb_dsts.bit0 SUSPSTS` = 1 → 总线挂起（主机没开始枚举 / 线只供电）；
      `usb_gotgctl.bit19 BSESVLD` = 0 → 核没识别到 VBUS；`usb_gintsts.bit0 CURMOD` 应为 0（device）；
      `usb_gccfg.bit16 PWRDWN` 应为 1（内部 FS 收发器上电）；`loop_count` 不涨 = 主循环卡死。
7. 无声排查顺序：`g_app_status.fault` → `DMA1_Stream0_IRQHandler` 是否进（DMA 的 TCIF 标志）→
   `hsai_BlockB3.hdmatx != NULL` → PD9 波形 → PCM5102A 模块的 FMT（=I2S）与 XSMT 跳线。
8. QSPI / SD 采样与音色功能属阶段 2/3，需先做第 4 节第 5、6 项。
9. **HardFault 现场记录（2026-10-18 晚新增）**：现象是「调试器里单步没事，放它自己全速跑必进
   `HardFault_Handler`」—— 单步时调试器会让中断不抢占（C_MASKINTS），所以这类故障几乎总是
   **中断上下文**里的问题（本工程首选嫌疑：音频 DMA `DMA1_Stream0` 中断，优先级 0；其次 OTG_FS）。
   现在 `HardFault_Handler` 第一件事是调 `App_HardFaultRecord()`（`Core/Src/main.c`，`naked` + 内联汇编，
   自己不压栈），把现场存进 **`g_fault`**（`.ram_section`，软复位也不会丢）：
   - 线程模式（`EXC_RETURN.bit2 = 1`）→ 硬件栈帧在 `PSP`，精确；
   - 中断里 → 栈帧压在 `MSP` 上，从 MSP 向上扫最多 64 个字，找「xPSR 的 T 位 = 1 且 IPSR ≠ 0、
     PC 落在 Flash」的那一帧。
   记录：`magic/count/CFSR/HFSR/MMFAR/BFAR/ICSR/EXC_RETURN/SP/frame_valid` + `r0-r3/r12/LR/PC/xPSR` +
   `MSP/PSP`。存完**不复位**，改为 PE3 **三短一长**循环闪（与心跳灯三种闪法都不同），方便直接挂调试器看。
   读法：`tools\status.ps1`（自己从 ELF 里找 `g_fault` 符号；老固件没有该符号也不报错），
   输出 CFSR 逐位解释、犯规地址、**出事时在哪个异常**、PC/LR 的源码行（`arm-none-eabi-addr2line`）。
   ⚠ 别在 `HardFault_Handler` 里用 `SCB->ICSR` 的 `VECTACTIVE` 判断"出事时在哪个异常" —— 它恒为 3
   （HardFault 自己）；必须用压栈 xPSR 的低 9 位：27 = DMA1_Stream0、117 = OTG_FS、130 = SAI3。
   ⚠ 内联汇编按名字引用的 C 函数**不能是 `static`**：GCC 看不见这个引用，会把它当未使用函数丢掉，
   链接报 `undefined reference`（已踩）。

## 8. USB-MIDI 枚举失败的真正根因（2026-10-18 定位并修复）

现象：设备管理器能建出节点，但 `usbaudio.sys` 启动失败（代码 10 / "I/O 请求已取消"）；固件侧
`usb_mounted = 0`、`usb_mount_count = 0`；**单步调试一切正常，全速运行必挂**。

根因是两个事实叠加：
1. USB-MIDI 的配置描述符里，两个端点描述符落在**奇数地址** —— `TUD_MIDI_DESCRIPTOR` 展开后
   MIDIStreaming 类描述符共 65 字节，`wTotalLength = 101`；实测 `desc_configuration` 在
   `0x08014588`，EP OUT 描述符在 `+0x49`、EP IN 在 `+0x5F`（都是奇数）。
2. `tu_edpt_packet_size()`（`Synth/third_party/tinyusb/src/common/tusb_types.h`）用
   `desc_ep->wMaxPacketSize` 读包长 → GCC 生成 `ldrh r3,[r1,#4]`，而 **Cortex-M7 的 LDRH 不支持
   非对齐访问** → `UsageFault(UNALIGNED)` → `HFSR.FORCED` → HardFault。主机发 `SET_CONFIGURATION`
   时崩在 `usbd_edpt_open()`（`usbd.c:1616` 的 `tu_edpt_validate()`）里，枚举就此中断。

修复（**本地补丁，升级 TinyUSB 时必须保留**）：`tu_edpt_packet_size()` 改成逐字节读，指针必须
`volatile` ——
```c
volatile const uint8_t* p = (volatile const uint8_t*) &desc_ep->wMaxPacketSize;
uint16_t const v = (uint16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8));
return tu_le16toh(v) & 0x7FF;
```
不加 `volatile` 时 `-O2` 会把两条 `ldrb` 又合并回一条 `ldrh`，等于没打补丁（实测踩过）。
验证：`arm-none-eabi-objdump -d <build>/.../usbd.c.obj` 里 `usbd_edpt_open` 应是
`ldrb r2,[r1,#4]` + `ldrb r3,[r1,#5]`。

实测结果：`fault = 0`、`usb_mounted = 1`、`usb_mount_count = 1`、`usb_irq_count = 107`、
`DCFG.DEVADDR = 30`；Windows 注册表
`HKLM\SYSTEM\CurrentControlSet\Enum\USB\VID_CAFE&PID_4020\0001` → "USB Composite Device"/`usbccgp`，
子节点 `…&MI_00` → "USB Audio Device"/`usbaudio`，两者都没有 `Problem` 值。

✅ 已修（2026-10-19）：`App_HardFaultRecord` 的 naked 序言改成 `mrs r1,msp` + `ldr r0,[r1,#4]`
—— 在 C 函数里 `mov r0,lr` 拿到的是 `bl` 的返回地址，不是 `EXC_RETURN`。同时
`App_FindFaultFrame()` 不再要求「IPSR≠0」（线程模式出事时 IPSR 就是 0），只校验 `xPSR.T=1`
且 `PC` 落在 flash。实测抓到 `EXC_RETURN=0xFFFFFFE9`、`PC=0x080035E6`（线程模式、主循环渲染时崩）。

## 10. 切音色必崩（UNALIGNED）：`synth_patch_t` 只有 1 字节对齐

现象：上位机点「切换音色」（Program Change）→ 板子立刻卡死（PE3 三短一长闪灯、声音停、
USB 不再响应）。`tools\status.ps1` 读到 `g_fault.count=1`、`CFSR=0x01000000`（UNALIGNED）、
`HFSR=0x40000000`（FORCED）。

定位过程（不接调试器也能做）：
1. openocd `dump_image` 从 `g_fault.msp` 起 256 B → `[MSP+4]=0xFFFFFFE9`（EXC_RETURN）、
   `[MSP+8..]` = r0/r1/r2/r3/r12/lr/pc/xpsr → **PC = 0x080035E6**。
2. `arm-none-eabi-addr2line -f -C -e <elf> 0x080035E6` → `RenderBlock(...)`，源码位置
   `Synth/src/core/engine.cpp:106`（`ApplyPatchIndex()` 被内联进来了）。
3. `objdump -d` 该处：`movs r3,#53`（= `sizeof(synth_patch_t)`）+ `mla r5,r3,r8,r5` +
   `ldr r0,[r5,#0]` —— 结构体整块拷贝用的是**字**访问，而 `e.bank[5]` 的地址是 `bank+265`（奇数）。

根因：`synth_patch_t` 的成员全是 `uint8_t/int8_t` + `char name[16]` ⇒ `sizeof = 53`（奇数）、
对齐 = 1。数组第 N 个元素地址 = `bank + 53N`，N 为奇数时落在奇数地址；`e.patch = e.bank[idx]`
这种结构体赋值被 GCC 展开成 `ldr/str` 整字拷贝，而 **Cortex-M7 不支持非对齐字访问** →
UsageFault(UNALIGNED) → 升级成 HardFault。所以 PC 0 正常、PC 1/3/5…（53N 为奇数）必崩。

修复（两处，缺一不可）：
1. `Synth/include/synth/patch.h`：`} __attribute__((aligned(4))) synth_patch_t;`
   —— `sizeof` 自动补到 56，数组每个元素都 4 字节对齐。
2. `Synth/src/core/engine.cpp`：两处结构体赋值改成 `memcpy()`
   （`:106` `e.patch = e.bank[idx]`、`:305` `e.patch = *rpp`）—— 外部传入的 patch 指针
   （`.syn` 文本解析出的缓冲区）不保证对齐，`memcpy` 对非对齐源是安全的。

验证（上板实测）：15 个音色全扫 + 400 条随机 MIDI + 240 条 NRPN + 15 轮满 polyphony 快速切音色
→ `fault=0`、`midi_rx=3025`、`loop_count` 持续增长、`g_fault.magic=0`。

⚠ 通用判据：**任何 `sizeof` 不是 4 的倍数的结构体，都不要整体赋值/按值传参**（GCC 会生成字访问）；
要么加 `aligned(4)`，要么一律 `memcpy`。本工程已踩两次：USB 端点描述符的 `wMaxPacketSize`
（奇数偏移）和这次的 `synth_patch_t`。

## 11. MIDI 解析加固（2026-10-19）

`Synth/src/midi/midi_parser.h` 原来把 `0xF1/0xF2/0xF3` 一律当 2 个数据字节，且让它们建立
running status —— 收到 MTC Quarter Frame/Song Select 后会多吃一个字节、后续数据字节被错解。
现在：
- 系统公共消息按真实长度解析（`0xF1`=1、`0xF2`=2、`0xF3`=1、`0xF6`=0），且**不建立 running status**；
- `0xF4/0xF5` 这类未定义消息直接丢弃并计数；
- 新增 `stray_bytes`（无状态字节的数据字节）与 `dropped_common` 计数，便于排查错位流；
- SysEx 仍整体丢弃（本工程不需要，`synth_load_fm_sysex()` 是 C API，不走 MIDI 输入）。

验证：发 `0xF1 0x40`、`0xF2 0x10 0x02`、`0xF3 0x07`、`0xF6`、`0xF8`、`0xFA`、`0xFC`、`0xFE`
与一个孤立数据字节 `0x40`，随后一条正常 note on/off 仍正常、`fault=0`。
