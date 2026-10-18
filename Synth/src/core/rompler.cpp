#include "rompler.h"

namespace ssynth {

namespace {

/* 取一个采样点（循环时在 [loop_start, loop_end) 内回绕），返回 −1..1 */
inline float Fetch(const int16_t *d, uint32_t frames, bool looping,
                   uint32_t loop_start, uint32_t loop_end, int64_t idx) {
    if (looping && loop_end > loop_start + 1u) {
        const int64_t le = (int64_t)loop_end;
        const int64_t len = (int64_t)loop_end - (int64_t)loop_start;
        /* 只把"越过循环末尾"的索引回绕回循环区；
           loop_start 之前的样点是第一次播放时的正常内容，不能回绕（否则会错读循环区）。 */
        if (idx >= le) idx -= len * ((idx - le) / len + 1);
    }
    if (idx < 0) idx = 0;
    if (idx >= (int64_t)frames) idx = (int64_t)frames - 1;
    return (float)d[idx] * (1.0f / 32768.0f);
}

/* 三次 Hermite（Catmull-Rom） */
inline float InterpCubic(const int16_t *d, uint32_t frames, bool looping,
                         uint32_t ls, uint32_t le, int64_t i0, float frac) {
    const float x0 = Fetch(d, frames, looping, ls, le, i0 - 1);
    const float x1 = Fetch(d, frames, looping, ls, le, i0);
    const float x2 = Fetch(d, frames, looping, ls, le, i0 + 1);
    const float x3 = Fetch(d, frames, looping, ls, le, i0 + 2);
    const float c1 = 0.5f * (x2 - x0);
    const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * frac + c2) * frac + c1) * frac + x1;
}

/* 线性 */
inline float InterpLinear(const int16_t *d, uint32_t frames, bool looping,
                          uint32_t ls, uint32_t le, int64_t i0, float frac) {
    const float x1 = Fetch(d, frames, looping, ls, le, i0);
    const float x2 = Fetch(d, frames, looping, ls, le, i0 + 1);
    return x1 + (x2 - x1) * frac;
}

}  // namespace

void Rompler::Init(float sr, uint8_t max_voices) {
    sr_ = sr;
    max_voices_ = (max_voices == 0u || max_voices > SYNTH_ROM_MAX_VOICES) ? SYNTH_ROM_MAX_VOICES : max_voices;
    for (int i = 0; i < SYNTH_ROM_MAX_VOICES; ++i) {
        voices_[i] = Voice{};
        voices_[i].env.Set(0.002f, 0.05f, 1.0f, sr_, 0.08f);
    }
    age_counter_ = 0;
    steals_ = 0;
}

void Rompler::SetInstrument(const synth_sample_instrument_t *inst) {
    inst_ = inst;
    if (inst_ == nullptr) Panic();
}

const synth_sample_zone_t *Rompler::PickZone(uint8_t note) const {
    if (inst_ == nullptr) return nullptr;
    for (uint8_t i = 0; i < inst_->zone_count && i < SYNTH_ROM_MAX_ZONES; ++i) {
        const synth_sample_zone_t &z = inst_->zones[i];
        if (z.data == nullptr || z.frames < 2u) continue;
        if (note >= z.low && note <= z.high) return &z;
    }
    /* 没有匹配区间时用第一个可用音区（方便单音区乐器） */
    for (uint8_t i = 0; i < inst_->zone_count && i < SYNTH_ROM_MAX_ZONES; ++i) {
        const synth_sample_zone_t &z = inst_->zones[i];
        if (z.data != nullptr && z.frames >= 2u) return &z;
    }
    return nullptr;
}

int Rompler::AllocVoice(uint8_t note) {
    /* 同音重触发 */
    for (int i = 0; i < (int)max_voices_; ++i) {
        if (voices_[i].active && !voices_[i].released && voices_[i].note == note) return i;
    }
    for (int i = 0; i < (int)max_voices_; ++i) {
        if (!voices_[i].active) return i;
    }
    int best = -1;
    uint32_t best_age = 0xFFFFFFFFu;
    for (int i = 0; i < (int)max_voices_; ++i) {
        if (voices_[i].released && voices_[i].age < best_age) { best_age = voices_[i].age; best = i; }
    }
    if (best < 0) {
        best_age = 0xFFFFFFFFu;
        for (int i = 0; i < (int)max_voices_; ++i) {
            if (voices_[i].age < best_age) { best_age = voices_[i].age; best = i; }
        }
    }
    if (best >= 0) ++steals_;
    return best < 0 ? 0 : best;
}

void Rompler::NoteOn(uint8_t note, uint8_t velocity) {
    const synth_sample_zone_t *z = PickZone(note);
    if (z == nullptr) return;

    const int idx = AllocVoice(note);
    Voice &v = voices_[idx];
    v.zone = z;
    v.note = note;
    v.velocity = velocity;
    v.active = 1;
    v.released = 0;
    v.age = ++age_counter_;
    v.pos = 0.0;

    const float semis = (float)note - (float)z->root;
    const float rate_ratio = (float)z->rate / sr_;
    v.step = (double)(exp2f(semis * (1.0f / 12.0f)) * rate_ratio);

    const float vel01 = clampf((float)velocity * (1.0f / 127.0f), 0.0f, 1.0f);
    const float vol = (inst_ != nullptr) ? clampf(inst_->volume, 0.0f, 1.0f) : 0.8f;
    v.gain = vol * z->gain * (0.35f + 0.65f * vel01);

    v.env.Set(0.002f, 0.05f, 1.0f, sr_, 0.08f);
    v.env.GateOn(true);
}

void Rompler::NoteOff(uint8_t note) {
    for (int i = 0; i < (int)max_voices_; ++i) {
        if (voices_[i].active && !voices_[i].released && voices_[i].note == note) {
            voices_[i].env.GateOff();
            voices_[i].released = 1;
        }
    }
}

void Rompler::AllNotesOff() {
    for (int i = 0; i < (int)max_voices_; ++i) {
        if (voices_[i].active) { voices_[i].env.GateOff(); voices_[i].released = 1; }
    }
}

void Rompler::Panic() {
    for (int i = 0; i < (int)max_voices_; ++i) { voices_[i].active = 0; voices_[i].released = 0; }
}

uint8_t Rompler::ActiveNotes() const {
    uint8_t n = 0;
    for (int i = 0; i < (int)max_voices_; ++i) if (voices_[i].active) ++n;
    return n;
}

void Rompler::Render(float *interleaved_lr, uint32_t frames) {
    if (!Ready() || ActiveNotes() == 0) return;

    for (int vi = 0; vi < (int)max_voices_; ++vi) {
        Voice &v = voices_[vi];
        if (!v.active || v.zone == nullptr) continue;

        const synth_sample_zone_t &z = *v.zone;
        const uint32_t loop_start = z.loop_start;
        const uint32_t loop_end = (z.loop != 0u && z.loop_end > z.loop_start + 1u) ? z.loop_end : z.frames;
        const bool looping = (z.loop != 0u) && (loop_end > loop_start + 1u);

        for (uint32_t i = 0; i < frames; ++i) {
            const float e = v.env.Process();
            if (!v.env.Active()) { v.active = 0; break; }
            if (v.pos >= (double)z.frames) {
                if (looping) v.pos = (double)loop_start;
                else { v.active = 0; break; }
            }
            const double ipos = v.pos;
            const int64_t i0 = (int64_t)ipos;
            const float frac = (float)(ipos - (double)i0);

            float raw = (z.interp != 0u)
                      ? InterpCubic(z.data, z.frames, looping, loop_start, loop_end, i0, frac)
                      : InterpLinear(z.data, z.frames, looping, loop_start, loop_end, i0, frac);

            /* 循环接缝交叉淡化：末尾 F 点与 loop_start 之前 F 点混合 */
            const uint32_t F = z.crossfade;
            if (looping && F > 0u && loop_start > F && (uint32_t)i0 + F >= loop_end) {
                const double prev = ipos - (double)(loop_end - loop_start);
                const int64_t p0 = (int64_t)prev;
                const float pf = (float)(prev - (double)p0);
                const float other = (z.interp != 0u)
                                  ? InterpCubic(z.data, z.frames, false, 0u, 0u, p0, pf)
                                  : InterpLinear(z.data, z.frames, false, 0u, 0u, p0, pf);
                ++cf_hits_;
                float a = (float)((uint32_t)i0 - (loop_end - F)) / (float)F;
                if (a < 0.0f) a = 0.0f;
                if (a > 1.0f) a = 1.0f;
                raw = raw * (1.0f - a) + other * a;
            }

            const float s = raw * v.gain * e;

            interleaved_lr[(size_t)i * 2u]      += s;
            interleaved_lr[(size_t)i * 2u + 1u] += s;

            v.pos += v.step;
            if (looping && v.pos >= (double)loop_end) {
                const double len = (double)loop_end - (double)loop_start;
                while (v.pos >= (double)loop_end) v.pos -= len;
            }
        }
    }
}

}  // namespace ssynth
