# CubeMX 配置清单（音频 = SAI3 Block B）

> 引脚依据：核心板原理图 + CubeMX 器件库 `STM32H750VBTx.xml`。
> **PD4 = SDMMC1_SW（microSD 卡检测）**，所以 SAI3 Block A（PD0/PD1/PD4/PD15）不可用，改用 **Block B**。

## 1. 时钟（RCC）
| 项 | 值 | 说明 |
|---|---|---|
| HSE | 25 MHz（有源晶振，旁路输入） | 音频时钟必须从这里派生 |
| PLL1 | M=5, N=192, P=2, Q=20 | SYSCLK = 480 MHz，**PLL1Q = 48.000 MHz（USB）** |
| PLL2 | M=5, N=39, FRACN=21076, P=8 | **24.576 MHz 给 SAI23 内核**（分数分频才精确） |
| VOS / 供电 | VOS0 + PWR_LDO_SUPPLY，FLASH_LATENCY_4 | 与参考工程一致 |
| AHB / APB | HCLK_DIV2，APB1/2/3/4 = DIV2 | HCLK 240 MHz |

**验收**：SAI 参数页 `Real Audio Frequency` 必须显示 **48.000 kHz**、`Error` **0.00%**。
（参考工程当年是 46.875 kHz / −2.34%，就是内核时钟取错导致。）

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

> PCM5102A 模块：FMT 跳线 = I2S，XSMT 释放（非静音）。若模块 SCK 不接，则它用 BCK 内部派生时钟（3 线模式）也能工作。

## 3. USB（TinyUSB，**不要**勾 USB_DEVICE 中间件）
- **从 PA11/PA12 上移除 UART4**；PA11 = USB_OTG_FS_DM、PA12 = USB_OTG_FS_DP（AF10，Device_Only）。
- USB 时钟 = PLL1Q 48.000 MHz。
- 不要启用 ST 的 USB Device 中间件 / `stm32h7xx_hal_pcd.c` / `ll_usb.c`（TinyUSB 自带 dwc2 驱动，省 flash）。
- 允许只打开 USB_OTG_FS 外设以便在时钟树里选 USB 时钟源；USB 时钟选择与引脚 AF 由 `Synth/port/platform_stm32.c` 兜底完成。

## 4. 存储
- **SDMMC1 + FatFs**：PC8/PC9/PC10/PC11 = D0..D3、PC12 = CK、PD2 = CMD、PD4 = SW（卡检测，可不用）。阶段 1 用于 `/synth/*.syn` 音色。
- **QUADSPI（Bank1，四线）**：PB2 = CLK、PB6 = NCS、PD11 = IO0、PD12 = IO1、PD13 = IO3、PE2 = IO2。阶段 1 可留空，为阶段 2/3（FM 溢出 / 采样存储 / XIP）预留，与 SAI3 不冲突。
- 备用：SPIx flash = PB3 = CLK、PB4 = MISO、PD7 = MOSI、PD6 = CS（阶段 3 可选）。

## 5. Cache / MPU
- 开启 **I-Cache 与 D-Cache**。
- 为音频 DMA 缓冲所在的 32KB 加一条 **MPU 非缓存区域**（推荐放 D2 SRAM：0x30000000 起 32KB；或 AXI SRAM 0x24000000 起 32KB）。
- ⚠ CubeMX 默认 MPU 把 0x60000000–0xDFFFFFFF 设为 NO_ACCESS，**将来用 QSPI 内存映射（0x90000000）必须改这条**。

## 6. 构建
- 把 `Synth/src`、`Synth/port` 加进 `target_sources`，include `Synth/include`、`Synth/src`、`Synth/port`。
- **引擎源文件单独 `-O2`**（HAL 保持原样），不要开 `-ffast-math`。
- 保留 USART1（PB14/PB15）作为 printf 调试口（可选，约 +2KB flash）。

## 7. flash 预算（实测参考）
| 组件 | 实测 |
|---|---|
| 纯 HAL 基线（H750VBT6_Demo） | 37.4 KB |
| HAL + SAI1 + ST USB Device AUDIO 类（UAC1.0，含 C++ 运行时） | 70.8 KB |
| HAL + FatFs + LCD + WM8978（WM8978_Player） | 73.6 KB |
| 目标：HAL + TinyUSB + FatFs + 引擎 | **≤ 128 KB** |
---

## 8. 应用层集成（CubeMX 生成驱动之后）

### 8.1 main.c
```c
MX_SAI3_Init();                  /* SAI3 Block B，见第 2 节 */
synth_port_init();               /* D-Cache / FPU FTZ / DWT / USB 时钟 / 平台钩子 */

synth_config_t cfg = { SYNTH_SR_DEFAULT, SYNTH_MAX_VOICES, synth_factory_bank, synth_factory_bank_size };
synth_init(&cfg);
synth_port_audio_start();        /* 填满两个半缓冲 + 启动 SAI3 循环 DMA */
synth_port_usb_init();           /* TinyUSB */
if (synth_port_sd_mount() != 0) { /* 可选：SD 音色不可用就用内置音色 */ }

while (1) {
    synth_port_usb_task();       /* tud_task() */
    synth_port_patch_service();  /* Program Change → /synth/NNN.syn */
    synth_service();
}
```

### 8.2 CMake
```cmake
# 引擎（-O2，HAL 保持原设置）
file(GLOB_RECURSE SYNTH_SOURCES "Synth/src/*.c" "Synth/src/*.cpp")
file(GLOB SYNTH_PORT_SOURCES  "Synth/port/*.c")
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${SYNTH_SOURCES} ${SYNTH_PORT_SOURCES})
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
  "Synth/include" "Synth/src" "Synth/src/core" "Synth/port")
set_source_files_properties(${SYNTH_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")

# TinyUSB（已 vendor，MIT，commit 3c6524e）
add_subdirectory(Synth/third_party/tinyusb/src ${CMAKE_BINARY_DIR}/tinyusb_src)
tinyusb_target_add(${CMAKE_PROJECT_NAME})
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
  Synth/third_party/tinyusb/src/portable/synopsys/dwc2/dcd_dwc2.c
  Synth/third_party/tinyusb/src/portable/synopsys/dwc2/dwc2_common.c
  Synth/port/usb_midi_tinyusb.c)
```

### 8.3 中断
- `OTG_FS_IRQHandler` 改为调用 TinyUSB 的中断处理（`tud_int_handler(0)`；老版本为 `dcd_int_handler(0)`，
  以 vendor 版本 `src/portable/synopsys/dwc2/dwc2_stm32.h` 导出的符号为准）。**不要**再启用 ST 的 USB Device 中间件。
- `HAL_SAI_TxHalfCpltCallback` / `HAL_SAI_TxCpltCallback` / `HAL_SAI_ErrorCallback` **已在
  `Synth/port/audio_out_sai.c` 实现，不要在别处重复实现**。

### 8.4 内存放置（重要）
- **采样池**（`Synth/port/sample_storage_sd.c` 的 `g_sample_pool`，默认 512KB）同样在 `.ram_section`；
  若连同 FX 池（192KB）一起超过 AXI SRAM 余量，可调小 `SYNTH_SAMPLE_POOL_BYTES`。
- 若将来用 **QSPI 内存映射**放采样：把 `z->data` 指向 0x90000000 的映射地址，并**放开 MPU 对该区的 NO_ACCESS**。
- 音频 DMA 缓冲（`Synth/port/audio_out_sai.c` 里的 `g_audio_buf`）与 FX 大缓冲池
  （`Synth/src/fx/fx_memory.cpp`，192KB）都在 `.ram_section` → 链接脚本的 `RAM`（0x24000000，512KB AXI SRAM）。
  参考工程已有该段；**不要**把它们放进 DTCM（0x20000000，DMA1/2 访问不到，且只有 128KB）。
- 若链接脚本没有 `.ram_section`，用 `-DSYNTH_AUDIO_RAM=` 关掉属性（缓冲会落到 .bss）。

### 8.5 flash 预算（阶段 1）
| 组成 | 预计 |
|---|---|
| HAL + FatFs + SD + SAI3 + DMA | 40–50 KB |
| TinyUSB（dwc2 + MIDI 类） | 10–15 KB |
| 引擎（波表在 RAM，含 12 音色 + FX + 琶音器 + 文本解析） | 25–35 KB |
| 合计 | **约 80–100 KB / 128 KB** |
