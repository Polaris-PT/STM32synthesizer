/* 音色数据结构与枚举：引擎与音色库共用，不依赖任何平台头文件。 */
#ifndef SYNTH_PATCH_H
#define SYNTH_PATCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNTH_SR_DEFAULT      48000u
#define SYNTH_BLOCK_FRAMES    128u     /* 每个 DMA 半缓冲的帧数 */
#define SYNTH_MAX_VOICES      16u
#define SYNTH_MAX_PATCHES     64u
#define SYNTH_PATCH_NAME_LEN  16u
#define SYNTH_BEND_SEMITONES  2.0f

/* 振荡器模型（音色里用 id 选择，新增模型只加枚举 + 一个实现文件） */
typedef enum {
    SYNTH_OSC_SAW = 0,
    SYNTH_OSC_SQUARE,
    SYNTH_OSC_PULSE,
    SYNTH_OSC_TRI,
    SYNTH_OSC_SINE,
    SYNTH_OSC_SUPERSAW,
    SYNTH_OSC_NOISE,
    SYNTH_OSC_COUNT
} synth_osc_model_t;

typedef enum {
    SYNTH_FILT_LP12 = 0,
    SYNTH_FILT_LP24,
    SYNTH_FILT_BP12,
    SYNTH_FILT_HP12,
    SYNTH_FILT_TYPE_COUNT
} synth_filter_type_t;

typedef enum {
    SYNTH_LFO_SINE = 0,
    SYNTH_LFO_TRI,
    SYNTH_LFO_SQUARE,
    SYNTH_LFO_SAMPLE_HOLD,
    SYNTH_LFO_SHAPE_COUNT
} synth_lfo_shape_t;

typedef enum {
    SYNTH_LFO_DEST_PITCH = 0,
    SYNTH_LFO_DEST_FILTER,
    SYNTH_LFO_DEST_AMP,
    SYNTH_LFO_DEST_PW,
    SYNTH_LFO_DEST_COUNT
} synth_lfo_dest_t;

/* 定长音色：可直接放进 flash 的 const 数组 */
typedef struct {
    uint8_t osc1_model;         /* synth_osc_model_t */
    uint8_t osc2_model;
    uint8_t osc1_pw;            /* 0..255 -> 0.05..0.95 */
    uint8_t osc2_pw;
    int8_t  osc2_semi;          /* -24..+24 半音 */
    int8_t  osc2_detune;        /* -64..+63 音分 */
    uint8_t osc1_level;         /* 0..255 */
    uint8_t osc2_level;
    uint8_t sub_level;
    uint8_t noise_level;

    uint8_t filter_type;        /* synth_filter_type_t */
    uint8_t cutoff;             /* 0..255 -> 指数映射 20Hz..18kHz */
    uint8_t resonance;          /* 0..255 */
    uint8_t keytrack;           /* 0..255 -> 0..1 键盘跟踪 */
    int8_t  filter_env_amt;     /* -128..127 */

    uint8_t amp_a, amp_d, amp_s, amp_r;   /* 0..255 曲线时间 */
    uint8_t flt_a, flt_d, flt_s, flt_r;

    uint8_t lfo_shape;          /* synth_lfo_shape_t */
    uint8_t lfo_rate;           /* 0..255 -> 0.05..20 Hz */
    uint8_t lfo_depth;          /* 0..255 */
    uint8_t lfo_dest;           /* synth_lfo_dest_t */

    uint8_t glide;              /* 0..255 -> 0..1s */
    uint8_t vel_amp;            /* 力度->音量 0..255 */
    uint8_t vel_cut;            /* 力度->截止 0..255 */

    uint8_t fx_chorus;          /* 0..255 混合 */
    uint8_t fx_delay;
    uint8_t fx_delay_fb;
    uint8_t fx_drive;

    uint8_t volume;             /* 0..255 */

    /* 引擎选择：0 = 减法（上面的字段），1 = DX7 FM（fm_voice 选音色）
       fm_voice: 0..N-1 = 内置 FM 音色；0xFF = 由 synth_load_fm_sysex() 载入的 RAM 音色 */
    uint8_t engine_type;
    uint8_t fm_voice;

    char    name[SYNTH_PATCH_NAME_LEN];
} synth_patch_t;

extern const synth_patch_t synth_factory_bank[];
extern const uint16_t      synth_factory_bank_size;

#ifdef __cplusplus
}
#endif
#endif /* SYNTH_PATCH_H */
