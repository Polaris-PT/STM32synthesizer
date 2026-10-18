/* 滤波器：TPT/ZDF 状态变量滤波器，支持 12/24dB 低通、带通、高通。 */
#ifndef SSYNTH_FILTER_H
#define SSYNTH_FILTER_H

#include "dsp_util.h"
#include "synth/patch.h"

namespace ssynth {

struct TptSvf {
    float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, k = 1.0f;
    float ic1 = 0.0f, ic2 = 0.0f;

    inline void Reset() { ic1 = ic2 = 0.0f; }
    inline void SetCoeffs(float cutoff_hz, float res, float sr) {
        const float nyq = sr * 0.5f;
        cutoff_hz = clampf(cutoff_hz, 10.0f, nyq * 0.95f);
        res       = clampf(res, 0.5f, 40.0f);
        const float g = tanf(kPi * cutoff_hz / sr);
        k  = 1.0f / res;
        const float denom = 1.0f + g * (g + k);
        a1 = 1.0f / denom;
        a2 = g * a1;
        a3 = g * a2;
    }
    inline void Process(float v0, float &lp, float &bp, float &hp) {
        const float v3 = v0 - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        lp = v2; bp = v1; hp = v0 - k * v1 - v2;
    }
};

struct FilterBlock {
    TptSvf s1, s2;
    uint8_t type = SYNTH_FILT_LP24;

    inline void Reset() { s1.Reset(); s2.Reset(); }
    inline void SetCoeffs(float cutoff_hz, float res, float sr, uint8_t t) {
        type = t;
        if (t == SYNTH_FILT_LP24) {
            const float r = sqrtf(res);      /* 两级级联，单级 Q 取 sqrt 使总响应与 12dB 相近 */
            s1.SetCoeffs(cutoff_hz, r, sr);
            s2.SetCoeffs(cutoff_hz, r, sr);
        } else {
            s1.SetCoeffs(cutoff_hz, res, sr);
        }
    }
    inline float Process(float x) {
        float lp, bp, hp;
        switch (type) {
            case SYNTH_FILT_LP24:
                s1.Process(x, lp, bp, hp);
                s2.Process(lp, lp, bp, hp);
                return lp;
            case SYNTH_FILT_BP12:
                s1.Process(x, lp, bp, hp);
                return bp;
            case SYNTH_FILT_HP12:
                s1.Process(x, lp, bp, hp);
                return hp;
            case SYNTH_FILT_LP12:
            default:
                s1.Process(x, lp, bp, hp);
                return lp;
        }
    }
};

}  // namespace ssynth
#endif /* SSYNTH_FILTER_H */
