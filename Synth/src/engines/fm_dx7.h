/* 阶段 2：DX7 六运算器 FM 引擎（Apache-2.0 的 msfa 核心 + 自研复音管理）。
   voice 数据 = 标准 DX7 VCED 155 字节（补 1 字节到 156），与 msfa 的 Dx7Note 期望一致。
   音频输出为 int32（约 2^28 满幅），本引擎转为 float 并累加到交织立体声。 */
#ifndef SSYNTH_FM_DX7_H
#define SSYNTH_FM_DX7_H

#include <stdint.h>

#include "dx7note.h"        /* msfa（third_party/msfa） */
#include "EngineMsfa.h"
#include "controllers.h"
#include "lfo.h"
#include "exp2.h"
#include "sin.h"
#include "freqlut.h"
#include "porta.h"

namespace ssynth {

constexpr int kDx7VoiceBytes = 156;
constexpr int kDx7MaxNotes   = 8;
constexpr int kDx7Block      = 64;      /* msfa 固定块长 _N_ */

/* VCED 字节偏移（标准 DX7 语音格式；op_idx 0=OP6 … 5=OP1） */
constexpr int kDx7OpBlock    = 21;
constexpr int kDx7PegRates   = 126;
constexpr int kDx7PegLevels  = 130;
constexpr int kDx7Algorithm  = 134;
constexpr int kDx7Feedback   = 135;
constexpr int kDx7OscKeySync = 136;
constexpr int kDx7LfoStart   = 137;     /* LFS,LFD,LPMD,LAMD,LFKS,LFW,LPMS */
constexpr int kDx7Transpose  = 144;     /* 0..48，中心 24 */
constexpr int kDx7Name       = 145;     /* 10 字节 */

inline int Dx7OpOffset(int op_idx) { return op_idx * kDx7OpBlock; }

class FmDx7Engine {
public:
    void Init(float sample_rate, uint8_t max_notes);
    void LoadVoice(const uint8_t *voice);       /* 156 字节 VCED */
    void NoteOn(uint8_t note, uint8_t velocity);
    void NoteOff(uint8_t note);
    void AllNotesOff();
    void Panic();
    void Render(float *interleaved_lr, uint32_t frames);
    void SetPitchBend(float semitones);         /* ±2 半音 */
    void SetModWheel(uint8_t value);            /* 0..127 */

    uint8_t  ActiveNotes() const;
    uint32_t Steals() const { return steals_; }
    bool     VoiceLoaded() const { return voice_loaded_; }
    void     SetOutputGain(float g) { gain_ = g; }

private:
    struct Slot {
        uint8_t  midi_note = 0;
        uint8_t  velocity = 0;
        uint8_t  active = 0;
        uint8_t  released = 0;
        uint32_t age = 0;
        uint32_t silent_blocks = 0;
    };

    int  AllocSlot();
    void FreeSlot(int idx);

    EngineMsfa   engine_;                  /* msfa 的六运算器渲染核心（FmCore 实现） */
    Dx7Note      notes_[kDx7MaxNotes];
    Slot         slots_[kDx7MaxNotes];
    Lfo          lfo_;
    Controllers  ctrls_;
    uint8_t      voice_[kDx7VoiceBytes];
    int32_t      block_buf_[kDx7Block];
    int32_t      note_buf_[kDx7Block];
    float        sr_ = 48000.0f;
    uint8_t      max_notes_ = kDx7MaxNotes;
    uint32_t     age_counter_ = 0;
    uint32_t     steals_ = 0;
    bool         voice_loaded_ = false;
    float        gain_ = 4.0f;                  /* 输出补偿（1/2^28 × gain） */
};

}  // namespace ssynth
#endif /* SSYNTH_FM_DX7_H */
