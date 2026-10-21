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
int  synth_port_sd_is_mounted(void);   /* 1 = 已挂载（RAM overlay 模块的 sd_present 用它） */
void synth_port_patch_service(void);
int  synth_port_patch_load_file(const char *path);
int  synth_port_patch_save_file(uint16_t index, const synth_patch_t *p);
/* 挂载成功后跑一次的读写自检 + 内置音色导出（把 15 个内置音色写成 /synth/NNN.syn 模板，
   已存在的文件不覆盖）。返回位图，含义见 patch_storage_sd.c 顶部注释。 */
uint32_t synth_port_sd_selftest(void);
uint32_t synth_port_sd_exported(void);   /* 本次新写出几个文件（新卡首启 = 15，之后 = 0） */
/* 写回 / 覆盖自检：把 000.syn 用改过的副本覆盖 → 读回确认生效 → 恢复原件（见 .c 里注释）。
   返回位图：bit0 覆盖写成功 / bit1 读回 cutoff = 测试值 / bit2 恢复成功 / bit3 恢复后读回 = 内置值。 */
uint32_t synth_port_sd_write_test(void);

/* 4b. 开机状态持久化（/synth/state.cfg）：记住 16 个通道各用哪个音色号。
   开机顺序：挂载 SD → synth_port_state_load() → 之后主循环里每圈 synth_port_state_service()。
   load 返回恢复的通道数（-1 = 没有文件 / 没挂载）；service 在索引变化静默 2 s 后自动落盘。 */
int      synth_port_state_load(void);
int      synth_port_state_save(void);
void     synth_port_state_service(void);
int      synth_port_state_load_result(void);   /* 给 status.ps1：-1 = 没文件，>=0 = 恢复的通道数 */
uint32_t synth_port_state_save_count(void);    /* 上电以来落盘次数 */

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

/* 5c. QSPI 采样库（格式见 docs/sample-library.md）：
   - probe/info/count 在目录区里读（0x90000000 起，内存映射）
   - load_pc 把库里挂在某个 Program Change 上的乐器零拷贝交给引擎
   - import_from_card 从 SD 的 /synth/samples.bin 导入（PC 用 tools/pack_samples.py 打包）
   - selftest 在内存里造一个小库走同一条导入路径（会覆盖 QSPI 上的真库） */
int      synth_port_lib_probe(void);
uint32_t synth_port_lib_serial(void);
uint32_t synth_port_lib_count(void);
int      synth_port_lib_info(uint32_t index, char *name16, uint8_t *pc, uint8_t *zone_count);
uint8_t  synth_port_lib_mismatch_limit(uint32_t index); /* 该乐器允许的最大失配半音（0 = 不限制） */
int      synth_port_lib_load_pc(uint8_t pc, char *name_out);
int      synth_port_lib_has_pc(uint8_t pc);     /* 1 = 库里挂着这个 PC（优先级高于卡上 .ins/.syn） */
int      synth_port_lib_service(void);          /* 主循环调用：主 PC 变了就零拷贝挂载库乐器 */
int      synth_port_lib_import_from_card(void);
int32_t  synth_port_lib_import_state(void);     /* 0 = 没导过；1 = 成功；负 = 错误码 */
uint32_t synth_port_lib_import_progress(void);  /* 已写入字节 */
uint32_t synth_port_lib_import_count(void);     /* 导入成功次数 */
int      synth_port_lib_selftest(void);

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
