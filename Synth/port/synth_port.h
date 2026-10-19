/* 应用层与 CubeMX 工程之间的接口：main.c 只需调用这几个函数。 */
#ifndef SYNTH_PORT_H
#define SYNTH_PORT_H

#include <stdint.h>
#include "synth/patch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1. 系统初始化后调用：D-Cache、FPU FTZ、DWT、USB 时钟、平台钩子 */
void synth_port_init(void);

/* 2. SAI3 Block B 循环 DMA 启动（内部先填满两个半缓冲） */
int  synth_port_audio_start(void);
void synth_port_audio_stop(void);
/* 2b. 主循环每轮调一次：预渲染环有空位就渲染一块。渲染绝不能放在音频中断里
       （128 帧 @48 kHz 只有 2.667 ms，复音一多就会撑爆优先级 0 的中断 → 断续/丢 MIDI） */
void synth_port_audio_pump(void);

/* 3. USB MIDI（TinyUSB）：初始化一次，主循环里轮询 task */
int  synth_port_usb_init(void);
void synth_port_usb_task(void);

/* 4. SD 卡音色：挂载一次 + 主循环里 service */
int  synth_port_sd_mount(void);
void synth_port_patch_service(void);
int  synth_port_patch_load_file(const char *path);
int  synth_port_patch_save_file(uint16_t index, const synth_patch_t *p);
/* 挂载成功后跑一次的读写自检 + 内置音色导出（把 15 个内置音色写成 /synth/NNN.syn 模板，
   已存在的文件不覆盖）。返回位图，含义见 patch_storage_sd.c 顶部注释。 */
uint32_t synth_port_sd_selftest(void);
uint32_t synth_port_sd_exported(void);   /* 本次新写出几个文件（新卡首启 = 15，之后 = 0） */

/* 5. 阶段 3：ROMpler 采样（16bit PCM WAV → RAM 池） */
void     synth_port_sample_pool_reset(void);
uint32_t synth_port_sample_pool_used(void);
uint32_t synth_port_sample_pool_size(void);
int      synth_port_sample_load_wav(const char *path, uint8_t root, uint8_t low, uint8_t high,
                                    uint8_t loop, uint32_t loop_start, uint32_t loop_end);
void     synth_port_sample_activate(void);
/* 载入 .ins 乐器清单（多音区）并切到 ROMpler；返回音区数 */
int      synth_port_instrument_load(const char *path);

/* 5b. 阶段 3：QSPI flash 内存映射采样（零拷贝，需先放开 MPU） */
void     synth_port_sample_qspi_reset(void);
int      synth_port_sample_add_qspi(uint32_t byte_offset, uint32_t frames, uint32_t rate,
                                    uint8_t root, uint8_t low, uint8_t high,
                                    uint8_t loop, uint32_t loop_start, uint32_t loop_end);
void     synth_port_sample_qspi_activate(const char *name);

/* 诊断计数 */
extern volatile uint32_t g_synth_audio_xruns;   /* 音频 DMA 欠载次数（0 = 健康） */
extern volatile uint32_t g_synth_audio_underruns; /* 预渲染环没填上、中断重复上一块的次数 */
extern volatile uint32_t g_synth_midi_rx_bytes; /* USB-MIDI 收到的字节数（弹键就涨） */
extern volatile uint32_t g_synth_usb_mount_count;  /* 主机完成枚举的次数（反复增长 = 主机在反复重枚举） */
extern volatile uint32_t g_synth_usb_umount_count; /* USB 掉线次数 */
/* 存活探针：连读两次比较 systick_ticks/loop_count 判断是「主循环卡死」还是「卡在中断里」 */
extern volatile uint32_t g_synth_systick_ticks;    /* SysTick 中断次数（1 kHz） */
extern volatile uint32_t g_synth_usb_isr_depth;    /* >0 = 正在 USB 中断里 */
extern volatile uint32_t g_synth_audio_isr_depth;  /* >0 = 正在音频 DMA 中断里渲染 */
extern volatile uint32_t g_synth_usb_irq_count;    /* OTG_FS 中断次数（诊断：恒为 0 = 核没中断） */
extern volatile uint32_t g_synth_usb_clk_rc;       /* USB 内核时钟配置的 HAL 返回值（0 = HAL_OK） */

#ifdef __cplusplus
}
#endif
#endif /* SYNTH_PORT_H */
