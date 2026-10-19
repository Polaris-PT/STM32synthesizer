#include "wavetable.h"

namespace ssynth {

/* 32KB 波表本体：每个振荡器采样都要读 → 放 DTCM（SYNTH_HOT 定义见 dsp_util.h）。
   运行期由 Build() 填充（Init 时统一构建），所以放 NOLOAD 段是安全的。 */
float  WaveTables::storage_[(int)WtShape::kCount * WaveTables::kSumSizes] SYNTH_HOT;
float *WaveTables::tables_[(int)WtShape::kCount][kWtMips];
bool   WaveTables::ready_ = false;

void WaveTables::Build(WtShape sh, int mip) {
    const int n = kSizes[mip];
    const int H = kHarmonics[mip];
    float    *dst = tables_[(int)sh][mip];

    for (int i = 0; i < n; ++i) dst[i] = 0.0f;

    /* 每个谐波用旋转器递推（复数旋转），复杂度 O(N·H)，无逐点 sinf */
    for (int k = 1; k <= H; ++k) {
        double amp;
        if (sh == WtShape::kSaw) {
            amp = 1.0 / (double)k;
        } else {
            if ((k & 1) == 0) continue;                       /* 三角只有奇次谐波 */
            const double sign = (((k - 1) >> 1) & 1) ? -1.0 : 1.0;
            amp = sign / (double)(k * k);
        }
        const double w  = (2.0 * 3.14159265358979323846 * (double)k) / (double)n;
        const double c  = cos(w);
        const double s  = sin(w);
        double re = 1.0, im = 0.0;
        for (int i = 0; i < n; ++i) {
            dst[i] += (float)(amp * im);
            const double nr = re * c - im * s;
            im = re * s + im * c;
            re = nr;
        }
    }

    /* 归一化峰值，避免层间音量跳变 */
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) { const float a = fabsf(dst[i]); if (a > peak) peak = a; }
    if (peak > 1e-6f) {
        const float g = 1.0f / peak;
        for (int i = 0; i < n; ++i) dst[i] *= g;
    }
}

void WaveTables::Init(float sr) {
    (void)sr;
    if (ready_) return;

    int offset = 0;
    for (int s = 0; s < (int)WtShape::kCount; ++s) {
        for (int m = 0; m < kWtMips; ++m) {
            tables_[s][m] = &storage_[offset];
            offset += kSizes[m];
        }
    }
    for (int s = 0; s < (int)WtShape::kCount; ++s) {
        for (int m = 0; m < kWtMips; ++m) Build((WtShape)s, m);
    }
    ready_ = true;
}

}  // namespace ssynth
