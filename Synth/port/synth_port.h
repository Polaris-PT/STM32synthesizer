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

/* 3. USB MIDI（TinyUSB）：初始化一次，主循环里轮询 task */
int  synth_port_usb_init(void);
void synth_port_usb_task(void);

/* 4. SD 卡音色：挂载一次 + 主循环里 service */
int  synth_port_sd_mount(void);
void synth_port_patch_service(void);
int  synth_port_patch_load_file(const char *path);
int  synth_port_patch_save_file(uint16_t index, const synth_patch_t *p);

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
extern volatile uint32_t g_synth_audio_xruns;

#ifdef __cplusplus
}
#endif
#endif /* SYNTH_PORT_H */
