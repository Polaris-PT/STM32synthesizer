#include "osc.h"

namespace ssynth {

void SuperSaw::SetFreq(float hz, float sr, float detune01) {
    for (int i = 0; i < kNum; ++i) {
        const float t = ((float)i / (float)(kNum - 1)) * 2.0f - 1.0f;   /* -1..+1 */
        const float cents = t * detune01 * 45.0f;                       /* ±45 音分 */
        o[i].SetFreq(hz * exp2f(cents / 1200.0f), sr);
        o[i].phase = (float)i * (1.0f / (float)kNum) * 0.37f;           /* 相位错开 */
    }
}

void OscModel::Configure(uint8_t m, float phase) {
    model = m;
    osc.Reset(phase);
    rng.s = 0x9e3779b9u ^ ((uint32_t)m * 2654435761u);
}

}  // namespace ssynth
