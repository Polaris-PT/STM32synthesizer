#include "arp.h"

namespace ssynth {

void Arp::Init(float sr) {
    sr_ = sr;
    Reset();
}

void Arp::Reset() {
    held_count_ = 0;
    step_ = 0;
    dir_ = 1;
    counter_ = 0;
    gate_left_ = 0;
    active_note_ = 0xFF;
    clk_count_ = 0;
    clk_accum_ = 0;
}

void Arp::NoteOn(uint8_t note, uint8_t vel) {
    for (uint8_t i = 0; i < held_count_; ++i) {
        if (held_[i].note == note) { held_[i].vel = vel; return; }
    }
    if (held_count_ >= kArpMaxHeld) return;
    held_[held_count_].note  = note;
    held_[held_count_].vel   = vel;
    held_[held_count_].order = order_counter_++;
    ++held_count_;
    SortHeld();
    if (counter_ == 0 && active_note_ == 0xFF) counter_ = 1;   /* 首个音立即起步 */
}

void Arp::NoteOff(uint8_t note) {
    for (uint8_t i = 0; i < held_count_; ++i) {
        if (held_[i].note == note) {
            for (uint8_t j = i; j + 1u < held_count_; ++j) held_[j] = held_[j + 1u];
            --held_count_;
            break;
        }
    }
    if (held_count_ == 0) { active_note_ = 0xFF; gate_left_ = 0; }
}

void Arp::AllNotesOff() {
    held_count_ = 0;
    active_note_ = 0xFF;
    gate_left_ = 0;
    step_ = 0;
}

void Arp::SortHeld() {
    /* “played” 模式按演奏顺序，其它按音高排序 */
    for (uint8_t i = 1; i < held_count_; ++i) {
        Held key = held_[i];
        int j = (int)i - 1;
        while (j >= 0) {
            const bool swap = (mode_ == kPlayed) ? (held_[j].order > key.order)
                                                 : (held_[j].note > key.note);
            if (!swap) break;
            held_[j + 1] = held_[j];
            --j;
        }
        held_[j + 1] = key;
    }
}

int Arp::PickStep() {
    if (held_count_ == 0) return -1;
    const int total = (int)held_count_ * (int)octaves_;
    if (total <= 0) return -1;
    switch (mode_) {
        case kDown:
            step_ = (step_ - 1 + total) % total;
            break;
        case kUpDown:
            step_ += dir_;
            if (step_ >= total) { step_ = total - 2; dir_ = -1; if (step_ < 0) step_ = 0; }
            else if (step_ < 0)  { step_ = 1; dir_ = 1; if (step_ >= total) step_ = 0; }
            break;
        case kRandom:
            rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
            step_ = (int)(rng_ % (uint32_t)total);
            break;
        case kPlayed:
        case kUp:
        default:
            step_ = (step_ + 1) % total;
            break;
    }
    const int idx  = step_ % (int)held_count_;
    const int oct  = step_ / (int)held_count_;
    int note = (int)held_[idx].note + oct * 12;
    if (note > 127) note = 127;
    return note;
}

void Arp::OnMidiClock() {
    if (!clock_sync_) return;
    ++clk_count_;
    clk_accum_ = 0;
    /* 每 24 个时钟 = 一个四分音符；div_ = 每四分音符步数 */
    const uint32_t ticks_per_step = (24u / (div_ ? div_ : 1u)) ? (24u / (div_ ? div_ : 1u)) : 1u;
    if ((clk_count_ % ticks_per_step) == 0u) {
        if (counter_ == 0u) counter_ = 1u;      /* 时钟到来时立刻排一步 */
    }
}

int Arp::AdvanceBlock(uint32_t frames, Event *out, int max_out) {
    if (!enabled_ || mode_ == kOff || held_count_ == 0) {
        if (active_note_ != 0xFF) {                 /* 手放开后要收尾 */
            if (max_out > 0) { out[0].type = 2; out[0].note = active_note_; out[0].vel = 0; }
            active_note_ = 0xFF;
            gate_left_ = 0;
            return 1;
        }
        return 0;
    }

    int n = 0;
    /* 门限结束 */
    if (active_note_ != 0xFF && gate_left_ <= frames) {
        if (n < max_out) { out[n].type = 2; out[n].note = active_note_; out[n].vel = 0; ++n; }
        active_note_ = 0xFF;
    }
    if (gate_left_ > frames) gate_left_ -= frames; else gate_left_ = 0;

    /* 步进计时 */
    if (counter_ <= frames) {
        counter_ = 0;
        const int note = PickStep();
        if (note >= 0 && active_note_ == 0xFF) {
            active_note_ = (uint8_t)note;
            active_vel_  = held_[step_ % (int)held_count_].vel;
            if (n < max_out) { out[n].type = 1; out[n].note = active_note_; out[n].vel = active_vel_; ++n; }
            const uint32_t period = clock_sync_ ? clk_period_ * (24u / (div_ ? div_ : 1u))
                                                : (uint32_t)(sr_ / rate_hz_);
            gate_left_ = (uint32_t)((float)period * gate_);
            counter_ = period;
        }
    } else {
        counter_ -= frames;
    }
    return n;
}

}  // namespace ssynth
