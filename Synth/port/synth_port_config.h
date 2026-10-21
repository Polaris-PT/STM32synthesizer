/* ===== 与 CubeMX 工程的唯一适配点：句柄名 / 缓冲 / 目录在这里改 ===== */
#ifndef SYNTH_PORT_CONFIG_H
#define SYNTH_PORT_CONFIG_H

#include "stm32h7xx_hal.h"

/* --- 音频：SAI3 Block B（PD8=SCK/BCK, PD9=SD/DIN, PD10=FS/LRCK, PD14=MCLK） --- */
/* CubeMX 生成的句柄名是 hsai_BlockB3（SAI3 Block B），不是 hsai3 —— 之前这里对不上，
   会导致链接期 undefined reference。若以后在 .ioc 里改了实例名，只改这一行。 */
#if defined(HAL_SAI_MODULE_ENABLED)
extern SAI_HandleTypeDef hsai_BlockB3;
#define SYNTH_SAI_HANDLE        hsai_BlockB3
#endif

#define SYNTH_PORT_BLOCK_FRAMES 128u        /* 每个半缓冲帧数（与引擎块一致） */
#define SYNTH_PORT_WORDS_PER_FRAME 2u       /* 2 slots × 32bit（立体声） */

/* 音频 DMA 缓冲必须放 AXI SRAM/D2 SRAM，不能放 DTCM（DMA1/2 访问不到） */
#define SYNTH_PORT_AUDIO_RAM    __attribute__((section(".ram_section")))

/* --- USB MIDI（TinyUSB，dwc2 驱动，PA11/PA12） --- */
#define SYNTH_PORT_USB_ENABLE   1

/* --- SD 卡音色目录（FatFs 路径） --- */
#define SYNTH_PORT_PATCH_DIR    "0:/synth"
#define SYNTH_PORT_PATCH_MAXLEN 4224u    /* 需容纳 4104 字节的 DX7 bank dump */

/* --- SD/FatFs 开关 ---
   CubeMX 里已经加了 SDMMC1(4bit) + FATFS，改成 1 就会编进 sample_storage_sd.c /
   patch_storage_sd.c；当前置 0（两者退化成空实现），先用 QSPI 采样 + 内置音色上板验证。
   置 1 之前确认两件事：① SDMMC1 中断优先级低于音频 DMA（音频 DMA1_Stream0 = 0）；
   ② AXI SRAM 放得下采样池（默认 512KB 池会和 FX 192KB 一起超出 512KB，
   见下面 SYNTH_PORT_SAMPLE_POOL_BYTES 的说明）。 */
#ifndef SYNTH_PORT_SD_ENABLE
#define SYNTH_PORT_SD_ENABLE    1
#endif

/* --- ROMpler 采样池（.ram_section，AXI SRAM）---
   AXI SRAM 512KB = FX 池 192KB + 音频缓冲 8KB + 采样池（本值）+ FatFs/SD 对象 + 余量。
   实测（SD 开启时）：池 256KB → AXI 用掉 523,648/524,288 B = 99.88%，只剩 640 B，任何新增
   都会链接失败；池 128KB → 约 75%，稳妥。所以默认 128KB（≈1.5 秒 44.1kHz 单声道 16bit）。
   要更大就自己改成 192KB（约 87%）并实测。QSPI 里的采样是零拷贝直读 0x90000000，
   不占这个池。SD 关闭时这个池整个不存在（采样文件退化为空实现）。 */
#ifndef SYNTH_PORT_SAMPLE_POOL_BYTES
#define SYNTH_PORT_SAMPLE_POOL_BYTES (128u * 1024u)
#endif

/* --- 调试串口 ---
   1 = 打开。走 **CubeMX 生成的 USART1**（PB14 = TX、PB15 = RX、AF4、115200 8N1、无 DMA；
   外设与波特率都在 Core/Src/usart.c 里，改波特率请改 .ioc 或 usart.c）。
   日志走 8 KB 环形缓冲 + 逐字节 IT 发送，**不阻塞实时路径**（可从 ISR 里调），
   并且接管 printf（syscalls.c 里的 _write 是 weak）。
   接线：PB14 → USB-TTL 模块的 RX，PB15 → 模块的 TX，GND 共地（3.3V 电平）。 */
#ifndef SYNTH_PORT_DEBUG_UART
#define SYNTH_PORT_DEBUG_UART   1
#endif
#define SYNTH_PORT_DEBUG_BAUD   115200u   /* 仅作记录：实际波特率由 usart.c 决定 */

/* 每个 USB-MIDI 事件包打一行（判断 note-off 有没有到）。默认关：115200 下每包一行会明显
   拖慢串口，只在查"音停不下来"这类问题时才置 1。 */
#ifndef SYNTH_PORT_MIDI_TRACE
#define SYNTH_PORT_MIDI_TRACE   0
#endif

/* --- HardFault 之后怎么办 ---
   0（默认）= 记录现场 + 串口打一行 + PE3 "三短一长"循环闪，**不复位**：现场留在 g_fault
              （.ram_section），用 tools\status.ps1 读得到；接着挂调试器排查也方便。
   1 = 记录完现场后软复位（g_fault 在 .ram_section，复位不丢），板子自动恢复发声 ——
       当作乐器用、不想插调试器时选它。 */
#ifndef SYNTH_PORT_FAULT_RESET
#define SYNTH_PORT_FAULT_RESET  0
#endif

/* --- QSPI 采样库自检（第三档）---
   1 = 开机时如果 QSPI 里没有有效库，就在**内存里造一个**小库（1 件乐器、挂 PC 120、
       2048 帧循环正弦）走同一条导入路径写进 QSPI，然后重新枚举/挂载。
   用途：手边还没有 PC 打包出来的 samples.bin 时，验证「擦除 → 页编程 → 目录 → 零拷贝播放」
   整条链路；发 PC 120 就能听到 93.75 Hz 的正弦。
   ⚠ 会覆盖 QSPI 上的真库（真库导入后请把它关掉）。默认 0。 */
#ifndef SYNTH_PORT_LIB_SELFTEST_ON_BOOT
#define SYNTH_PORT_LIB_SELFTEST_ON_BOOT 0
#endif

/* --- 一次性验证自检（2026-10-19 都已上板通过，所以默认关）---
   SYNTH_PORT_SD_WRITE_TEST   1 = 开机把 /synth/000.syn 用"改过的副本"覆盖写 → 读回确认卡上内容
                              生效 → 再写回内置原件（会真的写卡，约 200 ms）。要通过它确认
                              "卡上音色覆盖内置音色"时打开，看 g_app_status.sd_wtest == 0x0F。
   SYNTH_PORT_QSPI_WRITE_TEST 1 = 开机擦 QSPI 最后一个 4 KB 扇区（0x7FF000）→ 写图案 →
                              回内存映射读回比对（约 60 ms）。看 g_app_status.qspi_wtest == 0
                              且 qspi_wtest_rd == 0x514E5953。
   两个都只在验证期开；关掉能省 ~1.2 KB flash 和启动时间，也不折腾卡/闪存。 */
#ifndef SYNTH_PORT_SD_WRITE_TEST
#define SYNTH_PORT_SD_WRITE_TEST   0
#endif
#ifndef SYNTH_PORT_QSPI_WRITE_TEST
#define SYNTH_PORT_QSPI_WRITE_TEST 0
#endif

#endif /* SYNTH_PORT_CONFIG_H */
