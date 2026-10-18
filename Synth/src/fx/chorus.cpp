#include "chorus.h"
#include "fx_memory.h"

namespace ssynth {

bool Chorus::Init(uint32_t max_frames) {
    uint32_t n = 1u;
    while (n < max_frames) n <<= 1;
    buf_ = (int16_t *)FxAlloc(n * 2u * sizeof(int16_t));
    if (buf_ == nullptr) return false;
    size_ = n;
    base_f_  = 0.008f * 48000.0f;      /* 基准延迟 8ms（由 SetParams 按实际采样率改） */
    depth_f_ = 0.004f * 48000.0f;      /* 调制深度 4ms */
    Reset();
    return true;
}

void Chorus::Reset() {
    if (buf_ != nullptr) for (uint32_t i = 0; i < size_ * 2u; ++i) buf_[i] = 0;
    w_ = 0u;
    phase_ = 0.0f;
}

void Chorus::SetParams(float rate_hz, float depth01, float mix01, float sr) {
    inc_     = clampf(rate_hz, 0.01f, 10.0f) / sr;
    base_f_  = 0.008f * sr;
    depth_f_ = (0.001f + 0.005f * clampf(depth01, 0.0f, 1.0f)) * sr;
    mix_     = clampf(mix01, 0.0f, 1.0f);
}

}  // namespace ssynth
