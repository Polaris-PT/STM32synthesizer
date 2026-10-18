/* 琶音器 / 音序调度：内部 BPM 或 MIDI 时钟同步，块级调度。 */
#ifndef SSYNTH_ARP_H
#define SSYNTH_ARP_H

#include <stdint.h>

namespace ssynth {

constexpr int kArpMaxHeld = 16;

class Arp {
public:
    enum Mode { kOff = 0, kUp, kDown, kUpDown, kRandom, kPlayed };

    struct Event { uint8_t type; uint8_t note; uint8_t vel; };   /* type: 1=on, 2=off */

    void Init(float sr);
    void Reset();

    void SetEnabled(bool on) { enabled_ = on; if (!on) Reset(); }
    void SetMode(uint8_t m) { mode_ = (m > kPlayed) ? (uint8_t)kPlayed : m; enabled_ = (mode_ != kOff); }
    void SetRateHz(float hz) { rate_hz_ = (hz < 0.2f) ? 0.2f : ((hz > 30.0f) ? 30.0f : hz); }
    void SetGate(float g01) { gate_ = (g01 < 0.05f) ? 0.05f : ((g01 > 1.0f) ? 1.0f : g01); }
    void SetOctaves(uint8_t n) { octaves_ = (n < 1) ? 1 : ((n > 4) ? 4 : n); }
    void SetClockSync(bool on) { clock_sync_ = on; }
    void SetClockDiv(uint8_t steps_per_quarter) { div_ = (steps_per_quarter < 1) ? 1 : steps_per_quarter; }

    bool     Enabled() const { return enabled_; }
    uint8_t  Mode_()   const { return mode_; }
    uint8_t  HeldCount() const { return held_count_; }
    float    RateHz()  const { return rate_hz_; }

    void NoteOn(uint8_t note, uint8_t vel);
    void NoteOff(uint8_t note);
    void AllNotesOff();
    void OnMidiClock();                                        /* 0xF8, 24 ppqn */

    /* 每个渲染块调用一次；返回本块要执行的事件数（0..max_out） */
    int  AdvanceBlock(uint32_t frames, Event *out, int max_out);

private:
    struct Held { uint8_t note; uint8_t vel; uint32_t order; };

    void   SortHeld();
    int    PickStep();

    float    sr_ = 48000.0f;
    bool     enabled_ = false;
    bool     clock_sync_ = false;
    uint8_t  mode_ = kUp;
    uint8_t  div_ = 4;                     /* 每四分音符 4 步 = 16 分音符 */
    uint8_t  octaves_ = 1;
    float    rate_hz_ = 8.0f;
    float    gate_ = 0.6f;
    uint32_t counter_ = 0;                 /* 距下一步的样点数 */
    uint32_t gate_left_ = 0;
    uint8_t  active_note_ = 0xFF;
    uint8_t  active_vel_ = 0;
    uint8_t  step_ = 0;
    int8_t   dir_ = 1;
    /* 时钟同步 */
    uint32_t clk_count_ = 0;
    uint32_t clk_period_ = 6000;           /* 相邻 0xF8 的样点间隔（估计值） */
    uint32_t clk_accum_ = 0;

    Held     held_[kArpMaxHeld];
    uint8_t  held_count_ = 0;
    uint32_t order_counter_ = 0;
    uint32_t rng_ = 0x1234567u;
};

}  // namespace ssynth
#endif /* SSYNTH_ARP_H */
