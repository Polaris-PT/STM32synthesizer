/* 多样本 ROMpler：多音区映射 + 循环 + 线性插值重采样 + ADSR（RAM 常驻采样）。 */
#ifndef SSYNTH_ROMPLER_H
#define SSYNTH_ROMPLER_H

#include "dsp_util.h"
#include "env.h"
#include "synth/sample.h"

namespace ssynth {

class Rompler {
public:
    void Init(float sr, uint8_t max_voices);

    /* 设置乐器。指针需长期有效（引擎只读）；传 nullptr 表示清空。 */
    void SetInstrument(const synth_sample_instrument_t *inst);

    void NoteOn(uint8_t note, uint8_t velocity);
    void NoteOff(uint8_t note);
    void AllNotesOff();
    void Panic();
    void Render(float *interleaved_lr, uint32_t frames);

    uint8_t  ActiveNotes() const;
    uint32_t Steals() const { return steals_; }
    uint32_t CrossfadeHits() const { return cf_hits_; }   /* 诊断：交叉淡化分支命中次数 */
    bool     Ready() const { return inst_ != nullptr && inst_->zone_count > 0u; }

private:
    struct Voice {
        const synth_sample_zone_t *zone = nullptr;
        uint8_t  note = 60;
        uint8_t  velocity = 100;
        uint8_t  active = 0;
        uint8_t  released = 0;
        uint32_t age = 0;
        double   pos = 0.0;
        double   step = 1.0;
        float    gain = 1.0f;
        Adsr     env;
    };

    int  AllocVoice(uint8_t note);
    const synth_sample_zone_t *PickZone(uint8_t note) const;

    Voice    voices_[SYNTH_ROM_MAX_VOICES];
    const synth_sample_instrument_t *inst_ = nullptr;
    float    sr_ = 48000.0f;
    uint8_t  max_voices_ = SYNTH_ROM_MAX_VOICES;
    uint32_t age_counter_ = 0;
    uint32_t steals_ = 0;
    uint32_t cf_hits_ = 0;
};

}  // namespace ssynth
#endif /* SSYNTH_ROMPLER_H */
