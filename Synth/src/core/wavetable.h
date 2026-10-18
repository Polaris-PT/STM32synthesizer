/* 带限波表（mip 分层）：锯齿/三角按谐波数分层，避免混叠。
   表在 synth_init() 时生成（宿主瞬间，目标约 10ms），静态占用约 33KB。 */
#ifndef SSYNTH_WAVETABLE_H
#define SSYNTH_WAVETABLE_H

#include "dsp_util.h"

namespace ssynth {

constexpr int kWtMips = 9;

enum class WtShape { kSaw = 0, kTri, kCount };

class WaveTables {
public:
    static void Init(float sample_rate);
    static bool Ready() { return ready_; }

    static inline int SelectMip(float freq, float sr) {
        const float maxf = sr * 0.5f;
        for (int m = 0; m < kWtMips; ++m) {
            if (freq <= maxf / (float)kHarmonics[m]) return m;
        }
        return kWtMips - 1;
    }
    static inline float Read(WtShape sh, int mip, float phase) {
        const float *t = tables_[(int)sh][mip];
        const int    n = kSizes[mip];
        const float  p = (phase - floorf(phase)) * (float)n;
        const int    i = (int)p;
        const float  f = p - (float)i;
        const float  a = t[i & (n - 1)];
        const float  b = t[(i + 1) & (n - 1)];
        return a + (b - a) * f;
    }
    static constexpr int Harmonics(int mip) { return kHarmonics[mip]; }
    static constexpr int Size(int mip) { return kSizes[mip]; }

    /* 每层谐波数与表长：表长 ≈ 8×谐波数（2 的幂，下限 64） */
    static constexpr int kHarmonics[kWtMips] = {256, 128, 64, 32, 16, 8, 4, 2, 1};
    static constexpr int kSizes[kWtMips]     = {2048, 1024, 512, 256, 128, 64, 32, 64, 64};
    static constexpr int kSumSizes = 4192;

private:
    static float  storage_[(int)WtShape::kCount * kSumSizes];
    static float *tables_[(int)WtShape::kCount][kWtMips];
    static bool   ready_;
    static void   Build(WtShape sh, int mip);
};

}  // namespace ssynth
#endif /* SSYNTH_WAVETABLE_H */
