/* float <-> PCM 转换（音频 DMA 格式）。主机与目标共用，无 libm 依赖。 */
#ifndef SYNTH_PCM_H
#define SYNTH_PCM_H

#include <stdint.h>

/* 24bit 有效精度、左对齐到 32bit slot：SAI3 Data Size = 32 bits */
static inline int32_t synth_f2i32(float x)
{
    if (x >  0.999999f) x =  0.999999f;
    if (x < -1.0f)      x = -1.0f;
    return (int32_t)(x * 8388607.0f) * 256;
}

static inline float synth_i32_to_f(int32_t v)
{
    return (float)v * (1.0f / 2147483648.0f);
}

/* 16bit slot 备选格式 */
static inline int16_t synth_f2i16(float x)
{
    if (x >  0.99997f) x =  0.99997f;
    if (x < -1.0f)     x = -1.0f;
    return (int16_t)(x * 32767.0f);
}

#endif /* SYNTH_PCM_H */
