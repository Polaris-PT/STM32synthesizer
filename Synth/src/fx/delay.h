/* 立体声延迟：int16 环形缓冲 + 线性插值读，延迟时间平滑变化。 */
#ifndef SSYNTH_DELAY_H
#define SSYNTH_DELAY_H

#include "dsp_util.h"

namespace ssynth {

class StereoDelay {
public:
    bool Init(uint32_t max_frames);            /* 需 2 的幂；从 FX 池分配 2 通道 int16 */
    void Reset();
    void SetTimeSeconds(float seconds, float sr);
    void SetFeedback(float fb01);
    void SetMix(float mix01);
    bool Ready() const { return buf_ != nullptr; }

    inline void Process(float &l, float &r) {
        if (buf_ == nullptr || mix_ <= 0.0001f) return;
        delay_f_ += (target_f_ - delay_f_) * 0.0008f;          /* 读指针平滑，避免爆响 */
        const float dl = Read(delay_f_);
        const float dr = Read(delay_f_);
        const float wl = l + dl * fb_;
        const float wr = r + dr * fb_;
        buf_[2u * w_]     = (int16_t)(clampf(wl, -1.0f, 1.0f) * 32767.0f);
        buf_[2u * w_ + 1u]= (int16_t)(clampf(wr, -1.0f, 1.0f) * 32767.0f);
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
    float    delay_f_ = 0.0f, target_f_ = 0.0f, fb_ = 0.0f, mix_ = 0.0f;
};

}  // namespace ssynth
#endif /* SSYNTH_DELAY_H */
