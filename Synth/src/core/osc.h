/* 振荡器模型：带限波表（mip）实现锯齿/脉冲/三角/正弦，超锯，噪声。 */
#ifndef SSYNTH_OSC_H
#define SSYNTH_OSC_H

#include "dsp_util.h"
#include "wavetable.h"
#include "synth/patch.h"

namespace ssynth {

/* 波表振荡器：相位 0..1，按频率自动选择 mip 层 */
struct TableOsc {
    float phase = 0.0f;
    float inc   = 0.0f;
    int   mip   = kWtMips - 1;

    inline void Reset(float ph = 0.0f) { phase = ph; }
    inline void SetFreq(float hz, float sr) {
        inc = hz / sr;
        if (inc < 0.0f)  inc = 0.0f;
        if (inc > 0.49f) inc = 0.49f;
        mip = WaveTables::SelectMip(hz, sr);
    }
    inline float Advance() {
        const float p = phase;
        phase += inc;
        if (phase >= 1.0f) phase -= 1.0f;
        return p;
    }
    inline float Saw()  { return WaveTables::Read(WtShape::kSaw, mip, Advance()); }
    inline float Tri()  { return WaveTables::Read(WtShape::kTri, mip, Advance()); }
    inline float Sine() { return SineTable::Get(Advance()); }

    /* 脉冲 = 两个相位错开的锯齿之差（零均值），按占空比归一化峰值 */
    inline float Pulse(float pw) {
        const float p = Advance();
        float q = p - pw;
        if (q < 0.0f) q += 1.0f;
        const float d = WaveTables::Read(WtShape::kSaw, mip, p) - WaveTables::Read(WtShape::kSaw, mip, q);
        const float m = (pw > 0.5f) ? pw : (1.0f - pw);
        return d * (0.5f / m);
    }
    inline float Square(float pw) { return Pulse(pw); }
};

/* 7 振荡器超锯 */
struct SuperSaw {
    static constexpr int kNum = 7;
    TableOsc o[kNum];
    void SetFreq(float hz, float sr, float detune01);
    inline float Next() {
        float s = 0.0f;
        for (int i = 0; i < kNum; ++i) s += o[i].Saw();
        return s * (1.0f / 3.0f);
    }
};

/* 单个振荡器模型实例 */
struct OscModel {
    uint8_t  model = SYNTH_OSC_SAW;
    TableOsc osc;
    SuperSaw super;
    Rng      rng;

    void  Configure(uint8_t m, float phase);
    inline void SetFreq(float hz, float sr) {
        osc.SetFreq(hz, sr);
        if (model == SYNTH_OSC_SUPERSAW) super.SetFreq(hz, sr, 0.35f);
    }
    inline float Next(float pulse_width) {
        switch (model) {
            case SYNTH_OSC_SAW:      return osc.Saw();
            case SYNTH_OSC_SQUARE:   return osc.Square(0.5f);
            case SYNTH_OSC_PULSE:    return osc.Square(pulse_width);
            case SYNTH_OSC_TRI:      return osc.Tri();
            case SYNTH_OSC_SINE:     return osc.Sine();
            case SYNTH_OSC_SUPERSAW: return super.Next();
            case SYNTH_OSC_NOISE:    return rng.Next();
            default:                 return osc.Saw();
        }
    }
};

}  // namespace ssynth
#endif /* SSYNTH_OSC_H */
