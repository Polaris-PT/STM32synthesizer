/* 通用 DSP 工具：平滑、软限幅、噪声、正弦表、参数映射。 */
#ifndef SSYNTH_DSP_UTIL_H
#define SSYNTH_DSP_UTIL_H

#include <math.h>
#include <stdint.h>

/* 把变量放进 DTCM 的「热点段」（链接脚本 STM32H750xx_FLASH.ld 里的 .dtcm_bss）。
   内存策略：默认所有 .data/.bss 在 AXI SRAM（0x24000000），只有显式标注的常量表放 DTCM。
   DTCM（0x20000000, 128KB）与 CPU 同频、零等待，AXI SRAM 要过总线矩阵 —— 每采样都查的
   常量表值得搬进来（正弦表 4KB、波表 32KB）。
   两条约束：① 该段是 NOLOAD！启动代码不清零，由 Core/Src/main.c 的 App_ZeroDtcmBss() 清；
             ② DMA1/DMA2 访问不到 DTCM —— DMA 缓冲绝对不能标这个宏。 */
#ifndef SYNTH_HOT
#define SYNTH_HOT __attribute__((section(".dtcm_bss")))
#endif

namespace ssynth {

constexpr float kPi     = 3.14159265358979f;
constexpr float kTwoPi  = 6.28318530717959f;

inline float clampf(float v, float lo, float hi) { return (v < lo) ? lo : ((v > hi) ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline int   clampi(int v, int lo, int hi) { return (v < lo) ? lo : ((v > hi) ? hi : v); }

/* 一阶平滑（参数去拉链噪声） */
struct OnePole {
    float a = 1.0f, y = 0.0f;
    void SetTau(float tau_s, float sr) {
        if (tau_s <= 0.0f) { a = 1.0f; return; }
        a = 1.0f - expf(-1.0f / (tau_s * sr));
        a = clampf(a, 0.0f, 1.0f);
    }
    inline float Process(float x) { y += a * (x - y); return y; }
    inline void  Set(float v) { y = v; }
};

/* 软限幅：Padé 近似 tanh，|x|>3 直接饱和 */
inline float SoftClip(float x) {
    if (x < -3.0f) return -1.0f;
    if (x >  3.0f) return  1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* xorshift32 噪声 */
struct Rng {
    uint32_t s = 0x1234567u;
    inline float Next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float)(int32_t)s * (1.0f / 2147483648.0f);
    }
};

/* 共享正弦表（1024 点 + 线性插值），避免每样点调用 sinf */
class SineTable {
public:
    static void Init();
    static inline float Get(float phase) {
        float p = phase - floorf(phase);
        p *= kSizeF;
        const int   i = (int)p;
        const float f = p - (float)i;
        const float a = table_[i];
        const float b = table_[(i + 1) & kMask];
        return a + (b - a) * f;
    }
private:
    static constexpr int   kBits  = 10;
    static constexpr int   kSize  = 1 << kBits;
    static constexpr int   kMask  = kSize - 1;
    static constexpr float kSizeF = (float)kSize;
    static float SYNTH_HOT table_[kSize];   /* 4KB：振荡器每采样都查 → 放 DTCM */
};

/* 参数映射：音色里的 u8 → 物理量 */
inline float CutoffFromU8(uint8_t v) {                       /* 20 Hz .. 18 kHz 指数 */
    return 20.0f * powf(900.0f, (float)v * (1.0f / 255.0f));
}
inline float TimeFromU8(uint8_t v, float tmin, float tmax) { /* 指数时间 */
    return tmin * powf(tmax / tmin, (float)v * (1.0f / 255.0f));
}
inline float RateFromU8(uint8_t v) {                         /* 0.05 .. 20 Hz */
    return 0.05f * powf(400.0f, (float)v * (1.0f / 255.0f));
}
inline float LevelFromU8(uint8_t v) { return (float)v * (1.0f / 255.0f); }
inline float ResFromU8(uint8_t v) {                          /* 0.5 .. 15 (Q) */
    return 0.5f * powf(30.0f, (float)v * (1.0f / 255.0f));
}
inline float NoteToHz(float note) { return 440.0f * exp2f((note - 69.0f) * (1.0f / 12.0f)); }

}  // namespace ssynth
#endif /* SSYNTH_DSP_UTIL_H */
