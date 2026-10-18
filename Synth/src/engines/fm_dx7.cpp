#include "fm_dx7.h"
#include <string.h>
#include "../core/dsp_util.h"

namespace ssynth {

/* msfa 输出满幅约 2^28（见 Dexed：saturate(x>>4, 24, 9)/32768） */
static constexpr float kDx7Scale = 1.0f / 268435456.0f;
static constexpr int32_t kSilenceRaw = 4096;      /* 约 -96 dBFS */
static constexpr uint32_t kMaxReleaseBlocks = 20000u / 1u;   /* 释放上限（块数，约 20s @64） */

void FmDx7Engine::Init(float sample_rate, uint8_t max_notes) {
    sr_ = sample_rate;
    max_notes_ = (max_notes == 0 || max_notes > kDx7MaxNotes) ? kDx7MaxNotes : max_notes;

    /* msfa 的查表必须先初始化（否则 NULL 表指针 → 访问违例） */
    Exp2::init();
    Tanh::init();
    Sin::init();
    Freqlut::init(sr_);        /* 类名是 Freqlut */
    Env::init_sr(sr_);
    PitchEnv::init(sr_);
    Porta::init_sr(sr_);

    ctrls_.values_[kControllerPitch]      = 0x2000;
    ctrls_.values_[kControllerPitchRange] = 2;
    ctrls_.values_[kControllerPitchStep]  = 0;
    ctrls_.values_[kControllerPortamentoGlissando] = 0;
    ctrls_.masterTune = 0;
    ctrls_.modwheel_cc = 0;
    ctrls_.breath_cc = 0;
    ctrls_.foot_cc = 0;
    ctrls_.aftertouch_cc = 0;
    ctrls_.portamento_enable_cc = false;
    ctrls_.portamento_cc = 0;
    ctrls_.opSwitch = 0x3f;
    ctrls_.core = (FmCore *)&engine_;   /* Dx7Note::compute 通过它调用算法渲染 */
    ctrls_.refresh();

    lfo_.init(sr_);

    for (int i = 0; i < kDx7MaxNotes; ++i) {
        slots_[i] = Slot{};
        slots_[i].active = 0;
    }
    memset(voice_, 0, sizeof(voice_));
    memset(block_buf_, 0, sizeof(block_buf_));
    voice_loaded_ = false;
    age_counter_ = 0;
    steals_ = 0;
}

void FmDx7Engine::LoadVoice(const uint8_t *voice) {
    if (voice == nullptr) return;
    memcpy(voice_, voice, kDx7VoiceBytes);
    lfo_.reset(voice_ + kDx7LfoStart);
    voice_loaded_ = true;
}

int FmDx7Engine::AllocSlot() {
    /* 1. 空闲 */
    for (int i = 0; i < (int)max_notes_; ++i) {
        if (!slots_[i].active) return i;
    }
    /* 2. 释放中最旧 */
    int best = -1;
    uint32_t best_age = 0xFFFFFFFFu;
    for (int i = 0; i < (int)max_notes_; ++i) {
        if (slots_[i].released && slots_[i].age < best_age) { best_age = slots_[i].age; best = i; }
    }
    if (best >= 0) { ++steals_; return best; }
    /* 3. 最旧（偷音） */
    best_age = 0xFFFFFFFFu;
    for (int i = 0; i < (int)max_notes_; ++i) {
        if (slots_[i].age < best_age) { best_age = slots_[i].age; best = i; }
    }
    if (best >= 0) ++steals_;
    return best < 0 ? 0 : best;
}

void FmDx7Engine::FreeSlot(int idx) {
    slots_[idx].active = 0;
    slots_[idx].released = 0;
    slots_[idx].silent_blocks = 0;
}

void FmDx7Engine::NoteOn(uint8_t note, uint8_t velocity) {
    if (!voice_loaded_) return;
    const int idx = AllocSlot();
    const int play_note = (int)note + ((int)voice_[kDx7Transpose] - 24);
    notes_[idx].init(voice_, play_note < 0 ? 0 : (play_note > 127 ? 127 : play_note),
                     (int)velocity, play_note, 0, &ctrls_);
    slots_[idx].midi_note = note;
    slots_[idx].velocity  = velocity;
    slots_[idx].active    = 1;
    slots_[idx].released  = 0;
    slots_[idx].age       = ++age_counter_;
    slots_[idx].silent_blocks = 0;
    lfo_.keydown();
}

void FmDx7Engine::NoteOff(uint8_t note) {
    int best = -1;
    uint32_t best_age = 0;
    for (int i = 0; i < (int)max_notes_; ++i) {
        if (slots_[i].active && !slots_[i].released && slots_[i].midi_note == note && slots_[i].age > best_age) {
            best_age = slots_[i].age;
            best = i;
        }
    }
    if (best >= 0) {
        notes_[best].keyup();
        slots_[best].released = 1;
        slots_[best].silent_blocks = 0;
    }
}

void FmDx7Engine::AllNotesOff() {
    for (int i = 0; i < (int)max_notes_; ++i) {
        if (slots_[i].active && !slots_[i].released) {
            notes_[i].keyup();
            slots_[i].released = 1;
        }
    }
}

void FmDx7Engine::Panic() {
    for (int i = 0; i < (int)max_notes_; ++i) FreeSlot(i);
}

uint8_t FmDx7Engine::ActiveNotes() const {
    uint8_t n = 0;
    for (int i = 0; i < (int)max_notes_; ++i) if (slots_[i].active) ++n;
    return n;
}

void FmDx7Engine::SetPitchBend(float semitones) {
    const float clamped = clampf(semitones, -24.0f, 24.0f);
    ctrls_.values_[kControllerPitch] = 0x2000 + (int32_t)(clamped / 2.0f * 8192.0f);
}

void FmDx7Engine::SetModWheel(uint8_t value) {
    ctrls_.modwheel_cc = value;
    ctrls_.refresh();
}

void FmDx7Engine::Render(float *interleaved_lr, uint32_t frames) {
    if (frames == 0 || ActiveNotes() == 0) return;

    uint32_t done = 0;
    const float g = kDx7Scale * gain_;
    while (done < frames) {
        const uint32_t n = ((frames - done) < (uint32_t)kDx7Block) ? (frames - done) : (uint32_t)kDx7Block;

        const int32_t lfo_val = lfo_.getsample();
        const int32_t lfo_delay = lfo_.getdelay();

        for (int i = 0; i < kDx7Block; ++i) block_buf_[i] = 0;

        for (int s = 0; s < (int)max_notes_; ++s) {
            if (!slots_[s].active) continue;
            for (int i = 0; i < kDx7Block; ++i) note_buf_[i] = 0;
            notes_[s].compute(note_buf_, lfo_val, lfo_delay, &ctrls_);

            int32_t peak = 0;
            for (int i = 0; i < kDx7Block; ++i) {
                block_buf_[i] += note_buf_[i];
                const int32_t a = note_buf_[i] < 0 ? -note_buf_[i] : note_buf_[i];
                if (a > peak) peak = a;
            }
            if (slots_[s].released) {
                if (peak < kSilenceRaw) {
                    if (++slots_[s].silent_blocks > 2u) FreeSlot(s);
                } else {
                    slots_[s].silent_blocks = 0;
                }
                if (slots_[s].active && slots_[s].age + kMaxReleaseBlocks < age_counter_) FreeSlot(s);
            }
        }

        for (uint32_t i = 0; i < n; ++i) {
            float v = (float)block_buf_[i] * g;
            v = SoftClip(v);
            interleaved_lr[(size_t)(done + i) * 2u]      += v;
            interleaved_lr[(size_t)(done + i) * 2u + 1u] += v;
        }
        done += n;
    }
}

}  // namespace ssynth
