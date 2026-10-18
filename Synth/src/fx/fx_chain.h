/* 效果链：过载 → 合唱 → 延迟（在声部混音之后、主音量/限幅之前）。 */
#ifndef SSYNTH_FX_CHAIN_H
#define SSYNTH_FX_CHAIN_H

#include "dsp_util.h"
#include "synth/patch.h"
#include "chorus.h"
#include "delay.h"

namespace ssynth {

class FxChain {
public:
    bool Init(float sr);
    void Reset();
    void UpdateParams(const synth_patch_t &p, float sr);
    void Process(float *interleaved_lr, uint32_t frames);
    uint32_t MemoryUsed() const;
    bool ChorusReady() const { return chorus_.Ready(); }
    bool DelayReady() const { return delay_.Ready(); }

private:
    Chorus      chorus_;
    StereoDelay delay_;
    float sr_ = 48000.0f;
    float drive_gain_ = 1.0f;
    float drive_comp_ = 1.0f;
    float chorus_rate_ = 0.5f, chorus_depth_ = 0.5f;
};

/* 过载：预增益 + tanh 近似，带增益补偿 */
inline float Drive(float x, float gain, float comp) {
    return SoftClip(x * gain) * comp;
}

}  // namespace ssynth
#endif /* SSYNTH_FX_CHAIN_H */
