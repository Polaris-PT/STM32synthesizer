# CubeMX 配置清单（音频 = SAI3 Block B）

> 引脚依据：核心板原理图 + CubeMX 器件库 `STM32H750VBTx.xml`。
> **PD4 = SDMMC1_SW（microSD 卡检测）**，所以 SAI3 Block A（PD0/PD1/PD4/PD15）不可用，改用 **Block B**。

## 1. 时钟（RCC）
| 项 | 值 | 说明 |
|---|---|---|
| HSE | 25 MHz（有源晶振，旁路输入） | 音频时钟必须从这里派生 |
| PLL1 | M=5, N=192, P=2, Q=20 | SYSCLK = 480 MHz，**PLL1Q = 48.000 MHz（USB）** |
| PLL2 | M=5, N=108, FRACN=1101, P=22 | **24.57599987 MHz 给 SAI23 内核**（误差 −0.13 Hz ≈ −5 ppb）。VCO = 540.672 MHz（wide 档）、PLL2P 必须为偶数。SAI3 内核时钟源必须选 **PLL2P**（SAI23 Clock Mux = PLL2） |
| VOS / 供电 | VOS0 + PWR_LDO_SUPPLY，FLASH_LATENCY_4 | 与参考工程一致 |
| AHB / APB | HCLK_DIV2，APB1/2/3/4 = DIV2 | HCLK 240 MHz |

**验收**：SAI 参数页 `Real Audio Frequency` 必须显示 **48.000 kHz**、`Error` **0.00%**。
（参考工程当年是 46.875 kHz / −2.34%，就是内核时钟取错导致。）

> ⚠ 已生成的 `Core/Src/sai.c` 里，`HAL_SAI_MspInit()` 会把 `SAI23SEL` 写回 **PLL1Q**
> （`Sai23ClockSelection = RCC_SAI23CLKSOURCE_PLL`），而且 HAL 是在 MspInit **之后**才按当时的内核时钟算
> `MCKDIV` 的。所以 `Core/Src/main.c` 在 `MX_SAI3_Init()` 之后做了两件事：`App_ClockConfig_Audio()`
> 重新把 mux 指到 PLL2P，`App_ApplySaiMckdiv()` 按真实内核时钟重算 `MCKDIV`（=2）。
> CubeMX 里也要把时钟源选对，否则每次重新生成代码都会“看起来对、实际错”。

### 1.1 为什么 SAI 内核时钟必须是 12.288 MHz 的整数倍（48 MHz 为什么不行）

SAI 作主、MCLK 输出、`NoDivider = Enabled`（NODIV = 0）时，HAL 用的公式是
（`Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_sai.c:612-616`）：

```
MCKDIV = SAI_CK / (Fs × (OSR+1) × 256)        OSR = 0（MckOverSampling = Disabled）时
Fs     = SAI_CK / (MCKDIV × 256)     ⇒       SAI_CK 必须是 12.288 MHz 的整数倍
```

- 要 **48.000 kHz**：`SAI_CK = 12.288 MHz × MCKDIV` → 12.288（÷1）、**24.576（÷2）**、49.152（÷4）、98.304（÷8）…
- **48 MHz 不行**：48e6 / (48000 × 256) = 3.90625 → HAL 只能取 4
  （`tmpval = 39`，`39 % 10 = 9 > 8` → `Mckdiv = 4`，见 `stm32h7xx_hal_sai.c:649-655`）
  → MCK = 12 MHz → **Fs = 46.875 kHz，误差 −2.34%**（和参考工程当年一模一样）。
- 24.576 MHz 也拿不到整数分频（24.576 / 5 = 4.9152 不是整数），**必须用 PLL2 的 FRACN 逼近**：
  量化步进 = 5 MHz / 8192 = 610.35 Hz（到 PLL2P 还要 ÷P）；`M=5 / N=108 / FRACN=1101 / P=22`
  的余数最小 → −0.13 Hz（−5 ppb）。备选 `M=5 / N=78 / FRACN=5269 / P=16` → −3.6 Hz（−0.15 ppm），同样够用。
- CubeMX 操作路径：**Clock Configuration → PLL2 面板**填 M / N / FRACN / P，勾上 PLL2P 输出 →
  **SAI23 Clock Mux 选 PLL2P** → 回 SAI3 参数页，`Real Audio Frequency` 应为 **48.000 kHz**、`Error` **0.00%**。
- 就算 CubeMX 里没配对也不影响音质：`Core/Src/main.c` 的 `App_ClockConfig_Audio()` 会在 `MX_SAI3_Init()`
  之后再写一遍 PLL2 与 mux（生成代码的 `HAL_SAI_MspInit()` 会把 mux 抢回 PLL1Q），
  `App_ApplySaiMckdiv()` 再按真实内核时钟重算 MCKDIV（容差 ±0.1%，因为 FRACN 量化后必然差几 Hz）。

## 2. 音频输出：SAI3 Block B
| 项 | 值 |
|---|---|
| Mode | Master with Master Clock Out（主发送） |
| Protocol | I2S Standard（Philips） |
| Data Size | **32 bits**，Number of Slots = 2，Slot Size = 32 |
| MCLK | **输出使能**，MCLK = 12.288 MHz（256×fs） |
| Audio Frequency | 48 kHz（核对 Real = 48.000 kHz） |
| DMA | 循环模式，**WORD(32bit)**，MemInc 使能，半传输 + 传输完成中断都开 |
| 引脚 | **PD8 = SAI3_SCK_B（→ PCM5102A BCK）、PD9 = SAI3_SD_B（→ DIN）、PD10 = SAI3_FS_B（→ LRCK）、PD14 = SAI3_MCLK_B（→ SCK）** |
| NVIC | SAI3/DMA 中断优先级 **0**（必须高于 USB） |

> **生成现状（2026-10-18 检查）**：CubeMX 生成的 `MX_SAI3_Init()` 里协议是
> `SAI_PROTOCOL_DATASIZE_16BIT`（要 32bit），`HAL_SAI_MspInit()` 里**没有 DMA**（没有 `__HAL_LINKDMA`、
> 没有 DMA1 时钟、没有 NVIC），GPIO 速度是 `LOW`。这些都在 `Core/Src/main.c` 里软件补齐：
> `HAL_SAI_InitProtocol(..., 32BIT, 2)`、`App_SaiDmaFallback()`（DMA1 Stream0 + DMAMUX 请求 114 = SAI3_B，
> 循环/WORD/最高优先级/NVIC 0）、`App_SaiGpioSpeed()`（AF6 + VERY_HIGH）。
> 检测到 `hsai_BlockB3.hdmatx != NULL` 时兜底函数自动让位，所以以后在 CubeMX 里配上 DMA 不会冲突。
> CubeMX 侧仍建议按上表配好（省得每次重新生成都要靠软件兜）。

> PCM5102A 模块：FMT 跳线 = I2S，XSMT 释放（非静音）。若模块 SCK 不接，则它用 BCK 内部派生时钟（3 线模式）也能工作。


## 3. USB（TinyUSB，**不要**勾 USB_DEVICE 中间件）
- **从 PA11/PA12 上移除 UART4**；PA11 = USB_OTG_FS_DM、PA12 = USB_OTG_FS_DP（AF10，Device_Only）。
- USB 时钟 = PLL1Q 48.000 MHz。
- 不要启用 ST 的 USB Device 中间件 / `stm32h7xx_hal_pcd.c` / `ll_usb.c`（TinyUSB 自带 dwc2 驱动，省 flash）。
- 允许只打开 USB_OTG_FS 外设以便在时钟树里选 USB 时钟源；USB 时钟选择与引脚 AF 由 `Synth/port/platform_stm32.c` 兜底完成。

## 4. 存储
> **生成现状（2026-10-18 第二次生成之后）**：`Mcu.IP` = CORTEX_M7 / DEBUG / DMA / FATFS / MEMORYMAP /
> NVIC / QUADSPI / RCC / SAI3 / SDMMC1 / SYS —— SDMMC1 + FATFS 已配好、QUADSPI 参数已修正；
> **没有 USB_OTG_FS**（不需要：TinyUSB 的 dwc2 驱动直接接管，见 §3）。
- **SDMMC1 + FatFs**（已配置）：PC8/PC9/PC10/PC11 = D0..D3、PC12 = CK、PD2 = CMD、PD4 = SW（卡检测，已上拉）。阶段 1 用于 `/synth/*.syn` 音色。
  `Synth/port/synth_port_config.h` 的 `SYNTH_PORT_SD_ENABLE` 目前仍是 **0**
  （`sample_storage_sd.c` / `patch_storage_sd.c` 退化成空实现）—— 等音频/USB 上板验证通过再置 1；
  置 1 的代价与前置条件见 §5 和 §8.4（**cache 一致性仍未处理**）。
- **QUADSPI（Bank1，四线）**：PB2 = CLK、PB6 = NCS、PD11 = IO0、PD12 = IO1、PD13 = IO3、PE2 = IO2。
  生成参数已按本清单改好：`ClockPrescaler = 4`（240 MHz/5 = **48 MHz**）、`FlashSize = 23`（2²³ = 8 MB）、
  `SampleShifting = HALF_CYCLE`、`ChipSelectHighTime = 3 CYCLE`、六个脚的 GPIO 速度 = VERY_HIGH。
  **进内存映射模式 CubeMX 不会生成，只能手写** —— 已实现在 `Core/Src/main.c` 的
  `App_QspiEnterMemoryMapped()`（`main()` 第 5 步调用）：`0x66`+`0x99` 软复位 → `0x9F` 读 JEDEC ID
  （写进 `g_app_status.qspi_id`，W25Q64 应为 `0xEF4017`）→ 读 SR2 的 QE 位，为 0 就 `0x06` 写使能 +
  `0x31` 置位并等 WIP 清零 → `HAL_QSPI_MemoryMapped()` 走 `0xEB` 四线快读（模式字节 0x00、
  `APP_QSPI_DUMMY_CYCLES` = 4，读出来错位就改 6）。失败只置 `APP_FAULT_QSPI_MM`，不阻塞启动。
  `Synth/port/sample_storage_qspi.c` 只做零拷贝寻址（直接读 `0x90000000 + offset`）。
- 备用：SPIx flash = PB3 = CLK、PB4 = MISO、PD7 = MOSI、PD6 = CS（阶段 3 可选）。

## 5. Cache / MPU（Region0/1/2 都在 CubeMX 的 CORTEX_M7 → MPU 里配，生成进 `MPU_Config()`）
- 开 **I-Cache**（`main()` 里，CubeMX 模板原本就有）与 **D-Cache**（`synth_port_init()` 里，在 MPU 之后）。
- **Region 0**：4 GB 背景区，`SubRegionDisable = 0x87`（CubeMX 默认）：启用子区 3–6
  （`0x60000000–0xDFFFFFFF`）为 NO_ACCESS；关掉子区 0/1/2/7 → 内部 flash、DTCM/AXI SRAM、
  外设段（`0x40000000–0x5FFFFFFF`）走默认存储映射。
  ⚠ **QUADSPI 的寄存器在 `0x52005000`**（`stm32h750xx.h:2184 QSPI_R_BASE = D1_AHB1PERIPH_BASE + 0x5000`），
  属于被关掉的子区 2 → 寄存器访问不受 NO_ACCESS 影响；`0xA0001000` 是 **STM32F7** 的 QUADSPI 地址，
  H7 上 `0xA0000000` 是 Reserved，**不需要为它加 MPU region**。
- **Region 1**：`0x08000000` / 128 KB / TEX1 可缓存 / 特权只读 / 可执行 → 内部 flash。
- **Region 2**：`0x90000000` / 8 MB / **非缓存** / 特权只读 / 不可执行 → QSPI 零拷贝采样窗口。
  （CubeMX 生成的 Region2 复用了 Region1 的结构体、只改差异字段，所以 TEX/C/B/AP 都继承 Region1
  = 正好是我们要的值。）MPU 的仲裁规则是「编号大的 region 优先」（ARMv7-M / ST AN4838），
  所以 Region2 覆盖 Region0 的 NO_ACCESS。**`MPU_Config()` 必须早于任何 `0x90000000` 访问。**
- 其余走默认存储映射（`HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT)`）：AXI/D2/D3 = Normal WBWA、
  外设 = Device。
- 音频 DMA 缓冲在 AXI SRAM（可缓存）→ 每次渲染后由 `Synth/port/audio_out_sai.c` 的
  `audio_clean()` / `SCB_CleanDCache_by_Addr()` 写回。
- ⚠ **待办（启用 SD 之前）**：SDMMC 用芯片内部 IDMA，FATFS 对象与采样池虽然已在 AXI SRAM
  （IDMA 可达），但仍是**可缓存**的 → 要么给它们单独一个非缓存 MPU region，要么在 diskio
  读写前后做 `SCB_CleanInvalidateDCache_by_Addr`，否则会读到陈旧数据。详见 §8.4。
  **（2026-10-18 补充）"能不能编进来"已经不是问题**：把 `SYNTH_PORT_SD_ENABLE` 置 1 试编译
  +链接通过 —— FLASH 109,532 B (83.6%)、AXI SRAM 392,576 B (74.9%)（采样池默认已降到 128 KB；
  256 KB 时 AXI 99.88%、只剩 640 B）。剩下的只有上面的 cache 一致性与真机验证。

## 6. 构建（已实现，见根目录 `CMakeLists.txt`）
- 已把 `Synth/src/**/*.cpp`、`Synth/third_party/msfa/*.cpp`、`Synth/port/*.c` 加进 `target_sources`；
  include 有 `Synth/include`、`Synth/src`、`Synth/src/core`、`Synth/src/engines`、`Synth/port`、`Synth/third_party/msfa`、
  `Synth/third_party/tinyusb/src`。
- **引擎与端口层单独 `-O2`**（HAL 保持原样），没开 `-ffast-math`；默认构建类型 `RelWithDebInfo`（`-O2 -g`），
  `Debug`（`-O0`）下 128 KB flash 会紧张。
- `CMAKE_CXX_EXTENSIONS ON`（gnu++17）：vendored 的 `Synth/third_party/msfa/sin.cpp:38` 用了 `M_PI`，
  newlib 只在 `_GNU_SOURCE` 下暴露它，严格 `-std=c++17` 会报 `'M_PI' was not declared in this scope`。
- **USART1（PB14/PB15）** 的 printf 调试口未启用（`SYNTH_PORT_DEBUG_UART 0`），需要时约 +2 KB flash。

## 7. flash 预算（实测参考）
| 组件 | 实测 |
|---|---|
| 纯 HAL 基线（H750VBT6_Demo） | 37.4 KB |
| HAL + SAI1 + ST USB Device AUDIO 类（UAC1.0，含 C++ 运行时） | 70.8 KB |
| HAL + FatFs + LCD + WM8978（WM8978_Player） | 73.6 KB |
| 目标：HAL + TinyUSB + FatFs + 引擎 | **≤ 128 KB** |

**2026-10-18 实测**（`cmake -G "MinGW Makefiles"`，RelWithDebInfo = `-O2 -g`；SD 关闭、QSPI 采样未启用）：
FLASH **91,804 B / 128 KB (70.0%)**、DTCMRAM **46,336 B / 128 KB (35.4%)**（栈 8 KB + 堆 0.5 KB +
DTCM 热点表 36.75 KB）、AXI SRAM **247,424 B / 512 KB (47.2%)**（`.bss`/`.data` + `.ram_section`：
FX 池 192 KB + 音频 8 KB；SD 关闭时采样池整个不存在）。
**同一份代码把 `SYNTH_PORT_SD_ENABLE` 置 1**（SD 路径参与编译 + 链接）：FLASH **109,532 B (83.6%)**、
AXI SRAM **392,576 B (74.9%)**（多出采样池 128 KB + FatFs 对象）。
加入 FatFs/SD 之后 FLASH 比上一轮（84,160 B）涨了 7.7 KB（`ff.c` 14 KB + `sd_diskio`/`bsp_driver_sd`/
`fatfs_platform` + `ccsbcs`）；`-DCMAKE_BUILD_TYPE=Release` 还能再省约 6 KB。
---

## 8. 应用层集成（CubeMX 生成驱动之后）

### 8.1 main.c（`Core/Src/main.c`，`main()` 手写在 `USER CODE 4` 里）
> `.ioc` 里勾了 **Project Manager → Do not generate the main()**。注意这个开关**不能**让 CubeMX
> 不生成 `main.c`（它是入口文件，CubeMX 没有"不生成"的选项）——它只是跳过 `int main(void)` 本体，
> 其余生成区照旧按 `.ioc` 重写、`USER CODE` 区原样保留。所以 `main()` 只存在于 `USER CODE 4`：
> **代价是以后在 CubeMX 里新加外设后，要自己把新的 `MX_xxx_Init()` 补进 `main()`**。

```c
MPU_Config();                       /* 必须最先：0x90000000 要靠 Region2 放行 */
SCB_EnableICache(); HAL_Init(); SystemClock_Config();
App_ZeroDtcmBss();                  /* .dtcm_bss 是 NOLOAD，启动不清零 → 手动清 */
MX_DMA_Init();                      /* 必须早于 MX_SAI3_Init()：SAI 初始化时要把 DMA 流链上去 */
MX_GPIO_Init(); MX_QUADSPI_Init();
MX_SDMMC1_SD_Init();                /* 必须早于第一次 f_mount()（在 synth_port_sd_mount() 里） */
MX_FATFS_Init();                    /* 只是登记驱动（ff_gen_drv.c:39），顺序无所谓 */
App_InitAudioPath();                /* MCLK 补丁 + GPIO VERY_HIGH + SAI 时钟自检 */
if (App_QspiEnterMemoryMapped() != 0) App_Fault(APP_FAULT_QSPI_MM);
synth_port_init();                                     /* D-Cache / FPU FTZ / DWT / USB 时钟 / 平台钩子 */
synth_config_t cfg = { SYNTH_SR_DEFAULT, SYNTH_MAX_VOICES, synth_factory_bank, synth_factory_bank_size };
g_app_status.synth_init  = synth_init(&cfg);
g_app_status.audio_start = synth_port_audio_start();
g_app_status.usb_init    = synth_port_usb_init();
#if SYNTH_PORT_SD_ENABLE
g_app_status.sd_mount    = synth_port_sd_mount();       /* 失败不致命：用内置音色 */
#endif

while (1) {
    synth_port_usb_task();       /* tud_task()：MIDI 包在这里解析 → synth_midi_in() */
    synth_port_patch_service();  /* Program Change → /synth/NNN.syn（要读文件，只能主循环） */
    synth_service();
}
```
> 1 ms 心跳在 `Core/Src/stm32h7xx_it.c` 的 `SysTick_Handler()` 里调 `synth_tick_1ms()`；
> `OTG_FS_IRQHandler()` 调 `tud_int_handler(0)`；`DMA1_Stream0_IRQHandler()` 由 CubeMX 生成
> （`HAL_DMA_IRQHandler(&hdma_sai3_b)`）。它们都在 `USER CODE` 区内，CubeMX 重新生成会保留。
> 失败信息累加在 `g_app_status.fault`（位定义见 `main.c` 顶部），调试时先看这个变量。

### 8.2 CMake（实际做法）
```cmake
# C++17（gnu++17：msfa 需要 M_PI）+ 默认 RelWithDebInfo
set(CMAKE_CXX_STANDARD 17)  # + CMAKE_CXX_EXTENSIONS ON
project(${CMAKE_PROJECT_NAME} C CXX ASM)      # 原来只有 C ASM，C++ 要显式加

file(GLOB_RECURSE SYNTH_ENGINE_SOURCES CONFIGURE_DEPENDS "Synth/src/*.cpp" "Synth/third_party/msfa/*.cpp")
file(GLOB         SYNTH_PORT_SOURCES   CONFIGURE_DEPENDS "Synth/port/*.c")
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${SYNTH_ENGINE_SOURCES} ${SYNTH_PORT_SOURCES})
set_source_files_properties(${SYNTH_ENGINE_SOURCES} ${SYNTH_PORT_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")

# TinyUSB：官方 src/CMakeLists.txt 的 tinyusb_target_add() 只在子目录作用域可见，
# 根目录调用不到 → 直接列源文件（device 栈 + MIDI 类 + dwc2，省 flash）
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
  "${TINYUSB_SRC}/tusb.c" "${TINYUSB_SRC}/common/tusb_fifo.c" "${TINYUSB_SRC}/device/usbd.c"
  "${TINYUSB_SRC}/class/midi/midi_device.c"
  "${TINYUSB_SRC}/portable/synopsys/dwc2/dcd_dwc2.c"
  "${TINYUSB_SRC}/portable/synopsys/dwc2/dwc2_common.c")
```

**本机沙箱提示**：`cmake -G Ninja` 会在 `-- Detecting C compiler ABI info` 处挂住（cmake→ninja 子进程链，
最小工程也能复现，与本工程无关），改用
`cmake -S . -B build-mingw -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM=<mingw32-make> -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake`
可正常配置+构建（已实测 100% 通过）。

### 8.3 中断
- `OTG_FS_IRQHandler` 改为调用 TinyUSB 的中断处理（`tud_int_handler(0)`；老版本为 `dcd_int_handler(0)`，
  以 vendor 版本 `src/portable/synopsys/dwc2/dwc2_stm32.h` 导出的符号为准）。**不要**再启用 ST 的 USB Device 中间件。
- `HAL_SAI_TxHalfCpltCallback` / `HAL_SAI_TxCpltCallback` / `HAL_SAI_ErrorCallback` **已在
  `Synth/port/audio_out_sai.c` 实现，不要在别处重复实现**。

### 8.4 内存放置（重要）
- **策略（2026-10-18 起）**：`.data/.tdata/.tbss/.bss` 默认全部放 **AXI SRAM**（链接脚本的 `RAM`，
  `0x24000000`），**DTCM 只留给栈、堆和显式标注的热点**。理由：SDMMC 的内部 IDMA 和 DMA1/DMA2
  都访问不到 DTCM，而采样池/FX 池/音频缓冲只能在 AXI SRAM；`.bss` 一起搬过去后，DTCM 就空出来
  装热点表（实测只用了 46 KB / 128 KB）。
- **热点表**用 `SYNTH_HOT` 标注（定义在 `Synth/src/core/dsp_util.h`，展开成
  `__attribute__((section(".dtcm_bss")))`）：正弦表 `SineTable::table_`（4 KB）与波表
  `WaveTables::storage_`（32 KB）已标注。`.dtcm_bss` 是 **NOLOAD**（不占 flash、启动代码也不清零）
  → 由 `main()` 里的 `App_ZeroDtcmBss()` 清零，所以里面只能放「运行期由 Init 填、不依赖静态构造」
  的数组。**DMA 缓冲绝对不能标这个宏（DMA 访问不到 DTCM）。**
- **DMA 缓冲段 `.ram_section`**（`(NOLOAD)`，`>RAM`）：音频 DMA 半缓冲
  （`Synth/port/audio_out_sai.c` 的 `g_audio_buf`，8 KB）、FX 池（`Synth/src/fx/fx_memory.cpp`，192 KB）、
  SD 采样池（`Synth/port/sample_storage_sd.c` 的 `g_sample_pool`，**128 KB**，大小在
  `Synth/port/synth_port_config.h` 的 `SYNTH_PORT_SAMPLE_POOL_BYTES`；256 KB 时 AXI 会到 99.88%）、
  FATFS 对象（`Synth/port/patch_storage_sd.c` 的
  `g_fs`，含 512 B 扇区窗口，必须让 IDMA 够得着）。属性宏名是 **`SYNTH_PORT_AUDIO_RAM`**
  （`Synth/port/synth_port_config.h`），不是 `SYNTH_AUDIO_RAM`（后者在 `Synth/src/fx/fx_memory.h`，
  是引擎内部另一个宏）。
- 链接脚本 `STM32H750xx_FLASH.ld`：`.data/.tdata/.tbss/.bss` → `>RAM`；`.dtcm_bss (NOLOAD) : ALIGN(32)`
  → `>DTCMRAM`；`.ram_section (NOLOAD) : ALIGN(32)` → `>RAM`；`_Min_Stack_Size` 从 CubeMX 默认的
  `0x400` 提到 `0x2000`（8 KB）。⚠ 文件末尾那段 `/DISCARD/ libc.a libm.a libgcc.a` 已被删掉
  （留着会让 C++ 运行时/`memcpy`/软浮点全部链接不上），**不要让 CubeMX 覆盖这个文件**。
- 若将来用 QSPI 内存映射放采样：`z->data` 直接指向 `0x90000000` 映射地址（Region2 只读非缓存），
  无需额外 MPU 处理。
- ⚠ **待办（启用 SD 之前）**：`.ram_section` 仍是可缓存的，而 SDMMC 的 IDMA 会直接写这些缓冲 ——
  启用 `SYNTH_PORT_SD_ENABLE=1` 前要补 cache 维护（读写前后
  `SCB_CleanInvalidateDCache_by_Addr`）或给 FATFS 对象/采样缓冲单独一个非缓存 MPU region。

### 8.5 flash 预算（阶段 1）
| 组成 | 预计 |
|---|---|
| HAL + FatFs + SD + SAI3 + DMA | 40–50 KB |
| TinyUSB（dwc2 + MIDI 类） | 10–15 KB |
| 引擎（波表在 RAM，含 12 音色 + FX + 琶音器 + 文本解析） | 25–35 KB |
| 合计 | **约 80–100 KB / 128 KB** |
