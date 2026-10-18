#include "delay.h"
#include "fx_memory.h"

namespace ssynth {

bool StereoDelay::Init(uint32_t max_frames) {
    uint32_t n = 1u;
    while (n < max_frames) n <<= 1;             /* 取 2 的幂，便于掩码回绕 */
    buf_ = (int16_t *)FxAlloc(n * 2u * sizeof(int16_t));
    if (buf_ == nullptr) return false;
    size_ = n;
    Reset();
    return true;
}

void StereoDelay::Reset() {
    if (buf_ != nullptr) {
        for (uint32_t i = 0; i < size_ * 2u; ++i) buf_[i] = 0;
    }
    w_ = 0u;
    delay_f_ = target_f_;
}

void StereoDelay::SetTimeSeconds(float seconds, float sr) {
    float f = seconds * sr;
    const float maxf = (float)(size_ > 4u ? size_ - 4u : 1u);
    target_f_ = clampf(f, 1.0f, maxf);
    if (delay_f_ <= 0.0f) delay_f_ = target_f_;
}

void StereoDelay::SetFeedback(float fb01) { fb_ = clampf(fb01, 0.0f, 0.95f); }
void StereoDelay::SetMix(float mix01)      { mix_ = clampf(mix01, 0.0f, 1.0f); }

}  // namespace ssynth
