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
#define SYNTH_PORT_SD_ENABLE    0
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

/* 每个 USB-MIDI 事件包打一行（判断 note-off 有没有到；嫌吵/嫌慢改 0） */
#ifndef SYNTH_PORT_MIDI_TRACE
#define SYNTH_PORT_MIDI_TRACE   1
#endif

#endif /* SYNTH_PORT_CONFIG_H */
