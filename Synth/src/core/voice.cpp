#include "voice.h"

namespace ssynth {

void Voice::Init(float sr) {
    sr_ = sr;
    Reset();
}

void Voice::Reset() {
    amp_.Kill();
    flt_env_.Kill();
    filter_.Reset();
    osc1_.Configure(SYNTH_OSC_SAW, 0.0f);
    osc2_.Configure(SYNTH_OSC_SAW, 0.0f);
    sub_.Reset();
    noise_rng_.s = 0x2545F491u;
    steal_left_ = steal_total_ = 0;
    age_ = 0;
    lfo_phase_ = 0.0f;
}

void Voice::NoteOn(uint8_t note, uint8_t velocity, const synth_patch_t &p,
                   bool retrigger, float prev_hz, bool glide) {
    note_  = note;
    vel_   = velocity;
    vel01_ = clampf((float)velocity * (1.0f / 127.0f), 0.0f, 1.0f);
    hz_target_ = NoteToHz((float)note);

    const float glide_s = TimeFromU8(p.glide, 0.0005f, 1.5f);
    if (glide && prev_hz > 0.0f && glide_s > 0.002f && prev_hz != hz_target_) {
        hz_ = prev_hz;
        glide_coef_ = 1.0f - expf(-1.0f / (glide_s * sr_ * 0.30f));
    } else {
        hz_ = hz_target_;
        glide_coef_ = 1.0f;
    }

    osc1_.Configure(p.osc1_model, 0.0f);
    osc2_.Configure(p.osc2_model, 0.0f);
    sub_.Reset(0.0f);
    noise_rng_.s = 0x2545F491u ^ ((uint32_t)note * 2654435761u);

    osc1_lvl_ = LevelFromU8(p.osc1_level);
    osc2_lvl_ = LevelFromU8(p.osc2_level);
    sub_lvl_  = LevelFromU8(p.sub_level);
    noise_lvl_= LevelFromU8(p.noise_level);

    pw1_base_ = pw1_ = clampf(0.05f + LevelFromU8(p.osc1_pw) * 0.90f, 0.05f, 0.95f);
    pw2_base_ = pw2_ = clampf(0.05f + LevelFromU8(p.osc2_pw) * 0.90f, 0.05f, 0.95f);
    ratio2_ = exp2f(((float)p.osc2_semi + (float)p.osc2_detune * 0.01f) * (1.0f / 12.0f));

    flt_base_     = CutoffFromU8(p.cutoff);
    flt_keytrack_ = LevelFromU8(p.keytrack);
    flt_env_amt_  = (float)p.filter_env_amt * (1.0f / 128.0f);
    flt_res_      = ResFromU8(p.resonance);
    vel_cut_      = LevelFromU8(p.vel_cut);

    lfo_inc_   = RateFromU8(p.lfo_rate) / sr_;
    lfo_shape_ = p.lfo_shape;
    lfo_dest_  = p.lfo_dest;
    lfo_depth_ = LevelFromU8(p.lfo_depth);

    amp_.Set(TimeFromU8(p.amp_a, 0.0005f, 8.0f),
             TimeFromU8(p.amp_d, 0.005f, 8.0f),
             LevelFromU8(p.amp_s),
             sr_,
             TimeFromU8(p.amp_r, 0.005f, 12.0f));
    flt_env_.Set(TimeFromU8(p.flt_a, 0.0005f, 8.0f),
                 TimeFromU8(p.flt_d, 0.005f, 8.0f),
                 LevelFromU8(p.flt_s),
                 sr_,
                 TimeFromU8(p.flt_r, 0.005f, 12.0f));

    amp_.GateOn(retrigger);
    flt_env_.GateOn(retrigger);
    filter_.Reset();

    const float va = LevelFromU8(p.vel_amp);
    amp_gain_ = 0.22f * LevelFromU8(p.volume) * lerpf(1.0f, vel01_, va);

    steal_left_ = steal_total_ = 0;
    age_ = 0;
}

void Voice::NoteOff()  { amp_.GateOff(); flt_env_.GateOff(); }
void Voice::Kill()     { amp_.Kill(); flt_env_.Kill(); steal_left_ = steal_total_ = 0; }

void Voice::BeginSteal(uint32_t frames) {
    steal_total_ = steal_left_ = frames;
    steal_gain0_ = amp_.level;
    amp_.Kill();
    flt_env_.Kill();
}

void Voice::UpdateControl(const synth_patch_t &p, const GlobalParams &g, uint32_t chunk) {
    /* 滑音（指数逼近目标） */
    if (hz_ != hz_target_) {
        const float c = 1.0f - powf(1.0f - glide_coef_, (float)chunk);
        hz_ += (hz_target_ - hz_) * c;
        if (fabsf(hz_ - hz_target_) < 0.05f) hz_ = hz_target_;
    }

    /* LFO 先算，音高/脉宽/截止都要用；CC1 调制轮叠加到深度上 */
    const float depth = clampf(lfo_depth_ + g.lfo_depth_mod, 0.0f, 1.0f);
    lfo_phase_ += lfo_inc_ * (float)chunk;
    lfo_phase_ -= floorf(lfo_phase_);
    switch (lfo_shape_) {
        case SYNTH_LFO_TRI:
            lfo_val_ = 1.0f - 4.0f * fabsf(lfo_phase_ - 0.5f);
            break;
        case SYNTH_LFO_SQUARE:
            lfo_val_ = (lfo_phase_ < 0.5f) ? 1.0f : -1.0f;
            break;
        case SYNTH_LFO_SAMPLE_HOLD:
            if (lfo_phase_ < sh_last_phase_) lfo_sh_ = rng_.Next();     /* 每周期换一次 */
            sh_last_phase_ = lfo_phase_;
            lfo_val_ = lfo_sh_;
            break;
        default:
            lfo_val_ = SineTable::Get(lfo_phase_);
            break;
    }

    float base = hz_;
    if (lfo_dest_ == SYNTH_LFO_DEST_PITCH) {
        base *= exp2f(lfo_val_ * depth * 2.0f * (1.0f / 12.0f));        /* 最多 ±2 半音 */
    }
    base *= exp2f(g.bend * SYNTH_BEND_SEMITONES * (1.0f / 12.0f));

    osc1_.SetFreq(base, sr_);
    osc2_.SetFreq(base * ratio2_, sr_);
    if (p.sub_level) sub_.SetFreq(base * 0.5f, sr_);

    if (lfo_dest_ == SYNTH_LFO_DEST_PW) {
        const float d = lfo_val_ * depth * 0.45f;
        pw1_ = clampf(pw1_base_ + d, 0.05f, 0.95f);
        pw2_ = clampf(pw2_base_ - d, 0.05f, 0.95f);
    } else {
        pw1_ = pw1_base_;
        pw2_ = pw2_base_;
    }

    /* 滤波器包络按 chunk 推进（控制率足够，6kHz 更新率） */
    float fenv = flt_env_.level;
    for (uint32_t k = 0; k < chunk; ++k) fenv = flt_env_.Process();

    float cut = flt_base_;
    if (flt_keytrack_ > 0.0f) cut *= exp2f(flt_keytrack_ * ((float)note_ - 60.0f) * (1.0f / 12.0f));
    if (flt_env_amt_ != 0.0f) cut *= exp2f(flt_env_amt_ * fenv * 4.0f);
    if (vel_cut_ > 0.0f)      cut *= exp2f(vel_cut_ * (vel01_ - 0.7f) * 2.0f);
    if (g.cutoff_mod != 0.0f) cut *= exp2f(g.cutoff_mod * 3.0f);
    if (lfo_dest_ == SYNTH_LFO_DEST_FILTER) cut *= exp2f(lfo_val_ * depth * 2.0f);

    const float res = flt_res_ * (1.0f + clampf(g.res_mod, -0.8f, 3.0f));
    filter_.SetCoeffs(cut, res, sr_, p.filter_type);
}

void Voice::Render(float *out_lr, uint32_t frames, const synth_patch_t &p, const GlobalParams &g) {
    if (!amp_.Active() && steal_left_ == 0) return;

    uint32_t done = 0;
    while (done < frames) {
        const uint32_t chunk = ((frames - done) > kCtrlRate) ? kCtrlRate : (frames - done);
        UpdateControl(p, g, chunk);

        const float lfo_amp_gain = (lfo_dest_ == SYNTH_LFO_DEST_AMP)
                                 ? (1.0f - clampf(lfo_depth_ + g.lfo_depth_mod, 0.0f, 1.0f) * 0.5f * (1.0f - lfo_val_))
                                 : 1.0f;

        for (uint32_t i = 0; i < chunk; ++i) {
            const float aenv = amp_.Process();
            float sig = osc1_lvl_ * osc1_.Next(pw1_)
                      + osc2_lvl_ * osc2_.Next(pw2_)
                      + sub_lvl_   * sub_.Saw()
                      + noise_lvl_ * noise_rng_.Next();
            if (noise_lvl_ <= 0.0f && sub_lvl_ <= 0.0f) {
                /* 无噪声/子振时保持相位推进一致性由上面分支已处理 */
            }
            float y = filter_.Process(sig);
            float out = y * aenv * amp_gain_ * lfo_amp_gain;
            if (steal_left_ > 0) {
                out = y * steal_gain0_ * ((float)steal_left_ / (float)steal_total_);
                --steal_left_;
            }
            const uint32_t o = (done + i) * 2u;
            out_lr[o]     += out;
            out_lr[o + 1] += out;
        }
        done += chunk;
    }
    if (steal_left_ == 0 && steal_total_ > 0) {
        Kill();
        steal_total_ = 0;
    }
}

}  // namespace ssynth
