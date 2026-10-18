/* 立体声合唱：两路 LFO 调制延迟（相位相差 1/4 周期），int16 环形缓冲。 */
#ifndef SSYNTH_CHORUS_H
#define SSYNTH_CHORUS_H

#include "dsp_util.h"

namespace ssynth {

class Chorus {
public:
    bool Init(uint32_t max_frames);
    void Reset();
    void SetParams(float rate_hz, float depth01, float mix01, float sr);
    void SetMix(float mix01) { mix_ = clampf(mix01, 0.0f, 1.0f); }
    bool Ready() const { return buf_ != nullptr; }

    inline void Process(float &l, float &r) {
        if (buf_ == nullptr || mix_ <= 0.0001f) return;
        phase_ += inc_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        float p2 = phase_ + 0.25f;
        if (p2 >= 1.0f) p2 -= 1.0f;
        const float mod_l = SineTable::Get(phase_);
        const float mod_r = SineTable::Get(p2);
        const float dl = Read(base_f_ + depth_f_ * mod_l);
        const float dr = Read(base_f_ + depth_f_ * mod_r);
        buf_[2u * w_]      = (int16_t)(clampf(l, -1.0f, 1.0f) * 32767.0f);
        buf_[2u * w_ + 1u] = (int16_t)(clampf(r, -1.0f, 1.0f) * 32767.0f);
        w_ = (w_ + 1u) & (size_ - 1u);
        l = l * (1.0f - mix_) + dl * mix_;
        r = r * (1.0f - mix_) + dr * mix_;
    }

private:
    inline float Read(float d) const {
        float rp = (float)w_ - d;
        while (rp < 0.0f) rp += (float)size_;
        const uint32_t i0 = (uint32_t)rp;
        const float    f  = rp - (float)i0;
        const uint32_t i1 = (i0 + 1u) & (size_ - 1u);
        const float a = (float)buf_[2u * i0];
        const float b = (float)buf_[2u * i1];
        return (a + (b - a) * f) * (1.0f / 32767.0f);
    }

    int16_t *buf_ = nullptr;
    uint32_t size_ = 0u, w_ = 0u;
    float    phase_ = 0.0f, inc_ = 0.0f, base_f_ = 0.0f, depth_f_ = 0.0f, mix_ = 0.0f;
};

}  // namespace ssynth
#endif /* SSYNTH_CHORUS_H */
