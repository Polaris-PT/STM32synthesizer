/* 单个声部：2 振荡器 + 子振 + 噪声 → 滤波 → 幅度包络，含滑音/LFO/偷音淡出。 */
#ifndef SSYNTH_VOICE_H
#define SSYNTH_VOICE_H

#include <string.h>

#include "dsp_util.h"
#include "osc.h"
#include "filter.h"
#include "env.h"
#include "synth/patch.h"

namespace ssynth {

/* 全局实时调制（来自 CC / 弯音轮），由引擎每块更新 */
struct GlobalParams {
    float master_vol    = 0.8f;   /* 0..1 */
    float cutoff_mod    = 0.0f;   /* -1..+1（CC74/CC2） */
    float res_mod       = 0.0f;   /* -1..+1（CC71） */
    float lfo_depth_mod = 0.0f;   /* 0..1（CC1） */
    float bend          = 0.0f;   /* -1..+1 → ±SYNTH_BEND_SEMITONES */
};

class Voice {
public:
    void Init(float sr);
    void Reset();

    bool     Active()    const { return amp_.Active() || steal_left_ > 0; }
    bool     Releasing() const { return amp_.Releasing(); }
    uint8_t  Note()      const { return note_; }
    uint32_t Age()       const { return age_; }
    float    EnvLevel()  const { return amp_.level; }
    float    Hz()        const { return hz_; }
    void     SetAge(uint32_t a) { age_ = a; }
    void     AdvanceAge() { if (age_ < 0xFFFFFFF0u) ++age_; }

    /* channel：本声部归属的 MIDI 通道；声部保存一份音色快照，渲染时用自己的那份，
       这样同一声部池里可以同时存在不同通道、不同音色的音（多音色）。 */
    void NoteOn(uint8_t note, uint8_t velocity, const synth_patch_t &p,
                bool retrigger, float prev_hz, bool glide, uint8_t channel = 0);
    void NoteOff();
    void Kill();
    void BeginSteal(uint32_t frames);       /* 从当前电平淡出到 0，然后转为 idle */
    bool Stealing() const { return steal_left_ > 0; }

    uint8_t Channel() const { return chan_; }
    void    SetPatch(const synth_patch_t &p) { memcpy(&patch_, &p, sizeof(patch_)); }

    void Render(float *out_lr, uint32_t frames, const GlobalParams &g);

private:
    void UpdateControl(const synth_patch_t &p, const GlobalParams &g, uint32_t chunk);

    static constexpr uint32_t kCtrlRate = 32;  /* 控制率分块（样点）：32 → 1.5 kHz。
                                                  UpdateControl 里有 exp2f/tanf/sqrtf，原来 8 样点
                                                  一次（6 kHz）在 M7 上是纯浪费：16 声部时这一项
                                                  就吃掉一成多实时预算。 */

    synth_patch_t patch_{};     /* 本声部自己的音色快照（按通道映射） */
    uint8_t  chan_      = 0;
    float    sr_        = 48000.0f;
    uint8_t  note_      = 69;
    uint8_t  vel_       = 100;
    float    vel01_     = 1.0f;
    uint32_t age_       = 0;

    float    hz_ = 440.0f, hz_target_ = 440.0f, glide_coef_ = 1.0f;

    float    osc1_lvl_ = 1.0f, osc2_lvl_ = 0.0f, sub_lvl_ = 0.0f, noise_lvl_ = 0.0f;
    float    pw1_ = 0.5f, pw2_ = 0.5f, pw1_base_ = 0.5f, pw2_base_ = 0.5f;
    float    ratio2_ = 1.0f;
    float    amp_gain_ = 0.22f;
    float    flt_base_ = 1200.0f, flt_keytrack_ = 0.0f, flt_env_amt_ = 0.0f, flt_res_ = 1.0f, vel_cut_ = 0.0f;

    float    lfo_phase_ = 0.0f, lfo_inc_ = 0.0f, lfo_val_ = 0.0f, lfo_sh_ = 0.0f, lfo_depth_ = 0.0f;
    uint8_t  lfo_shape_ = SYNTH_LFO_SINE, lfo_dest_ = SYNTH_LFO_DEST_PITCH;
    float    sh_last_phase_ = 0.0f;

    OscModel   osc1_, osc2_;
    TableOsc   sub_;
    Rng        noise_rng_;
    FilterBlock filter_;
    Adsr       amp_, flt_env_;

    uint32_t steal_left_ = 0, steal_total_ = 0;
    float    steal_gain0_ = 0.0f;
    Rng      rng_;
};

}  // namespace ssynth
#endif /* SSYNTH_VOICE_H */
