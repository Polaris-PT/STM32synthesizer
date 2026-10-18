/* 包络：ADSR（attack 线性、decay/release 指数），支持快速释放（偷音）。 */
#ifndef SSYNTH_ENV_H
#define SSYNTH_ENV_H

#include "dsp_util.h"

namespace ssynth {

struct Adsr {
    enum Stage { kIdle = 0, kAttack, kDecay, kSustain, kRelease };

    float   level   = 0.0f;
    float   sustain = 0.7f;
    float   a_inc   = 0.01f;
    float   d_coef  = 0.001f;
    float   r_coef  = 0.001f;
    uint8_t stage   = kIdle;

    void Set(float a_s, float d_s, float s, float sr, float r_s) {
        sustain = clampf(s, 0.0f, 1.0f);
        a_inc   = (a_s <= 0.0005f) ? 1.0f : (1.0f / (a_s * sr));
        d_coef  = CoefFromTime(d_s, sr);
        r_coef  = CoefFromTime(r_s, sr);
    }
    static float CoefFromTime(float t, float sr) {
        if (t <= 0.0005f) return 1.0f;
        return 1.0f - expf(-4.6f / (t * sr));      /* 4.6 ≈ -60 dB */
    }
    void GateOn(bool retrigger) {
        if (retrigger || stage == kIdle) level = 0.0f;
        stage = kAttack;
    }
    void GateOff() { if (stage != kIdle) stage = kRelease; }
    void Kill() { stage = kIdle; level = 0.0f; }

    inline float Process() {
        switch (stage) {
            case kAttack:
                level += a_inc;
                if (level >= 1.0f) { level = 1.0f; stage = kDecay; }
                break;
            case kDecay:
                level += (sustain - level) * d_coef;
                if (sustain <= 0.0008f && level < 0.0004f) { level = 0.0f; stage = kIdle; }
                else if (level - sustain < 0.0006f) { level = sustain; stage = kSustain; }
                break;
            case kSustain:
                level = sustain;
                if (sustain <= 0.0002f) { level = 0.0f; stage = kIdle; }
                break;
            case kRelease:
                level -= level * r_coef;
                if (level < 0.0002f) { level = 0.0f; stage = kIdle; }
                break;
            default:
                level = 0.0f;
                break;
        }
        return level;
    }
    inline bool Active()    const { return stage != kIdle; }
    inline bool Releasing() const { return stage == kRelease; }
};

}  // namespace ssynth
#endif /* SSYNTH_ENV_H */
