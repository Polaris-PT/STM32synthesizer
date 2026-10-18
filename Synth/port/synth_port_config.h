/* ===== 与 CubeMX 工程的唯一适配点：句柄名 / 缓冲 / 目录在这里改 ===== */
#ifndef SYNTH_PORT_CONFIG_H
#define SYNTH_PORT_CONFIG_H

#include "stm32h7xx_hal.h"

/* --- 音频：SAI3 Block B（PD8=SCK/BCK, PD9=SD/DIN, PD10=FS/LRCK, PD14=MCLK） --- */
#if defined(HAL_SAI_MODULE_ENABLED)
extern SAI_HandleTypeDef hsai3;             /* CubeMX 生成；若名字不同改这里 */
#define SYNTH_SAI_HANDLE        hsai3
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

/* --- 调试 --- */
#define SYNTH_PORT_DEBUG_UART   0

#endif /* SYNTH_PORT_CONFIG_H */
