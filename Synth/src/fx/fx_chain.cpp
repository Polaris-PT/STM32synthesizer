#include "fx_chain.h"
#include "fx_memory.h"

namespace ssynth {

bool FxChain::Init(float sr) {
    sr_ = sr;
    const bool ok_c = chorus_.Init((uint32_t)(0.060f * sr));    /* 60ms 合唱缓冲 */
    const bool ok_d = delay_.Init((uint32_t)(0.60f * sr));      /* 0.6s 延迟缓冲 */
    chorus_.SetParams(0.5f, 0.5f, 0.0f, sr);
    delay_.SetTimeSeconds(0.25f, sr);
    delay_.SetFeedback(0.35f);
    delay_.SetMix(0.0f);
    return ok_c && ok_d;
}

void FxChain::Reset() {
    chorus_.Reset();
    delay_.Reset();
}

void FxChain::UpdateParams(const synth_patch_t &p, float sr) {
    sr_ = sr;
    drive_gain_ = 1.0f + LevelFromU8(p.fx_drive) * 6.0f;
    drive_comp_ = 1.0f / (1.0f + LevelFromU8(p.fx_drive) * 1.6f);

    const float c = LevelFromU8(p.fx_chorus);
    chorus_rate_  = 0.18f + 4.0f * c;
    chorus_depth_ = 0.35f + 0.65f * c;
    chorus_.SetParams(chorus_rate_, chorus_depth_, c * 0.85f, sr);

    const float d = LevelFromU8(p.fx_delay);
    delay_.SetTimeSeconds(0.12f + 0.38f * LevelFromU8(p.fx_delay_fb), sr);   /* 反馈量也影响时间，音色更宽 */
    delay_.SetFeedback(LevelFromU8(p.fx_delay_fb) * 0.85f);
    delay_.SetMix(d * 0.75f);
}

void FxChain::Process(float *interleaved_lr, uint32_t frames) {
    const bool need_pair = (drive_gain_ > 1.001f) || chorus_.Ready() || delay_.Ready();
    if (!need_pair) return;
    for (uint32_t i = 0; i < frames; ++i) {
        float l = interleaved_lr[2u * i];
        float r = interleaved_lr[2u * i + 1u];
        if (drive_gain_ > 1.001f) {
            l = Drive(l, drive_gain_, drive_comp_);
            r = Drive(r, drive_gain_, drive_comp_);
        }
        chorus_.Process(l, r);
        delay_.Process(l, r);
        interleaved_lr[2u * i]      = l;
        interleaved_lr[2u * i + 1u] = r;
    }
}

uint32_t FxChain::MemoryUsed() const { return FxPoolUsed(); }

}  // namespace ssynth
