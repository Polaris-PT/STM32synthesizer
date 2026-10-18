/* 合成器引擎公开 API（C ABI）。上层（port 层 / 宿主程序）只使用这里。 */
#ifndef SYNTH_SYNTH_H
#define SYNTH_SYNTH_H

#include <stdint.h>
#include "patch.h"
#include "sample.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 平台钩子：目标端在 Synth/port/ 里实现并安装；不安装时引擎跳过 CPU 统计。 */
typedef struct {
    uint32_t (*cycles_now)(void);   /* 自由运行周期计数（DWT->CYCCNT） */
    uint32_t (*cpu_hz)(void);       /* CPU 主频 Hz */
} synth_platform_hooks_t;

void     synth_set_platform_hooks(const synth_platform_hooks_t *hooks);

typedef struct {
    uint32_t             sample_rate;      /* 必须等于 48000，其它值返回错误 */
    uint8_t              max_voices;       /* 1..SYNTH_MAX_VOICES */
    const synth_patch_t *factory_bank;     /* NULL 时用内置表 */
    uint16_t             factory_bank_size;
} synth_config_t;

typedef struct {
    uint16_t active_voices;      /* 当前发声声部数 */
    uint16_t peak_voices;        /* 历史峰值 */
    uint32_t midi_bytes;         /* 收到的 MIDI 字节数 */
    uint32_t midi_dropped;       /* 环形缓冲溢出丢弃字节数 */
    uint32_t notes_on;           /* 统计 */
    uint32_t steals;             /* 偷音次数 */
    uint32_t unknown_cc;
    uint32_t sysex_dropped;
    uint32_t render_peak_cycles; /* 单块渲染最大 CPU 周期（上板用 DWT，宿主为 0） */
    uint32_t render_last_cycles;
    uint32_t patch_index;
    uint8_t  sustain;
    uint8_t  mono_mode;
} synth_stats_t;

int      synth_init(const synth_config_t *cfg);
void     synth_deinit(void);

/* 音频渲染：interleaved L/R。f32 用于宿主/离线，i32 用于上板 DMA（24bit 左对齐）。 */
void     synth_render_f32(float   *interleaved_lr, uint32_t frames);
void     synth_render_i32(int32_t *interleaved_lr, uint32_t frames);

/* MIDI 字节流入口：可在 USB 回调或 ISR 中调用（内部为无锁环形缓冲）。 */
void     synth_midi_in(const uint8_t *bytes, uint32_t len);

/* 1ms 心跳（琶音器、统计、参数平滑兜底），主循环或 SysTick 调用。 */
void     synth_tick_1ms(void);
/* 主循环服务：音色加载、统计上报等；不得在 ISR 中调用。 */
void     synth_service(void);

void     synth_all_notes_off(void);
void     synth_panic(void);                        /* 立即静音（保留参数） */

/* 音色/参数控制（也可由 Program Change / CC 驱动） */
int      synth_set_patch(uint16_t index);
int      synth_set_patch_ptr(const synth_patch_t *patch);   /* 例如从 SD 加载后 */
uint16_t synth_get_patch_index(void);
void     synth_note_on(uint8_t note, uint8_t velocity);
void     synth_note_off(uint8_t note);
void     synth_set_channel_filter(int8_t channel);/* -1 = 全部通道（默认） */

/* 阶段 2：从 DX7 .syx 数据载入 FM 音色（32 音色 bank 或单音色 dump）。
   index 选择 bank 中的第几个（0 起）；成功后当前音色切换为 FM（engine_type=1, fm_voice=0xFF）。
   返回载入的音色总数（<0 为错误）。 */
int      synth_load_fm_sysex(const uint8_t *data, uint32_t len, uint8_t index);
/* 直接用 155/156 字节 VCED 设置 RAM FM 音色 */
void     synth_set_fm_voice_bytes(const uint8_t *vced);

/* 阶段 3：设置 ROMpler 乐器（采样需 RAM 常驻、长期有效）。inst 为 NULL 则清空。
   设置后需把当前音色的 engine_type 设为 2（见 patch.h）才会发声。 */
void     synth_set_sample_instrument(const synth_sample_instrument_t *inst);
void     synth_set_engine_type(uint8_t engine_type);   /* 0=减法 1=FM 2=ROMpler */
void     synth_set_master_volume(float vol01);

void     synth_get_stats(synth_stats_t *out);
uint32_t synth_cpu_load_permille(void);

#ifdef __cplusplus
}
#endif
#endif /* SYNTH_SYNTH_H */
