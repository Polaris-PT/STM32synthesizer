/* 采样（多样本 ROMpler）数据结构：单声道 16bit、RAM 常驻。
   由 port 层从 SD / QSPI flash 载入后交给 synth_set_sample_instrument()。 */
#ifndef SYNTH_SAMPLE_H
#define SYNTH_SAMPLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNTH_ROM_MAX_ZONES  8
#define SYNTH_ROM_MAX_VOICES 8

typedef struct {
    const int16_t *data;        /* 单声道 16bit 采样数据（RAM 常驻，长期有效） */
    uint32_t frames;            /* 采样点数 */
    uint32_t rate;              /* 采样率 Hz（重采样用） */
    uint8_t  root;              /* 原始音高（MIDI 音号） */
    uint8_t  low;               /* 键盘范围低 */
    uint8_t  high;              /* 键盘范围高 */
    uint8_t  loop;              /* 1 = 循环 */
    uint8_t  pad[3];
    uint32_t loop_start;
    uint32_t loop_end;          /* 0 表示到采样末尾 */
    float    gain;              /* 0..2 */
    uint32_t crossfade;         /* 循环接缝交叉淡化样点数（0 = 关闭），建议 64..512 */
    uint8_t  interp;            /* 0 = 线性插值，1 = 三次 Hermite */
    uint8_t  pad2[3];
} synth_sample_zone_t;

typedef struct {
    synth_sample_zone_t zones[SYNTH_ROM_MAX_ZONES];
    uint8_t zone_count;
    uint8_t pad[3];
    float   volume;             /* 0..1 */
    char    name[16];
} synth_sample_instrument_t;

#ifdef __cplusplus
}
#endif
#endif /* SYNTH_SAMPLE_H */
