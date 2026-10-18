#include "fm_voices.h"
#include <string.h>

namespace ssynth {

void Dx7SetOp(uint8_t *v, int op_idx,
              uint8_t r1, uint8_t r2, uint8_t r3, uint8_t r4,
              uint8_t l1, uint8_t l2, uint8_t l3, uint8_t l4,
              uint8_t out_level, uint8_t coarse, uint8_t detune,
              uint8_t kvs, uint8_t rs, uint8_t ams) {
    uint8_t *o = v + Dx7OpOffset(op_idx);
    o[0] = r1; o[1] = r2; o[2] = r3; o[3] = r4;      /* EG 速率 R1-R4 */
    o[4] = l1; o[5] = l2; o[6] = l3; o[7] = l4;      /* EG 电平 L1-L4 */
    o[8] = 0; o[9] = 0; o[10] = 0; o[11] = 0; o[12] = 0;  /* 键盘电平缩放 */
    o[13] = rs;      /* 速率缩放 */
    o[14] = ams;     /* 幅度调制灵敏度 */
    o[15] = kvs;     /* 键速灵敏度 */
    o[16] = out_level;
    o[17] = 0;       /* 0 = 比率模式 */
    o[18] = coarse;
    o[19] = 0;       /* 频率细调 */
    o[20] = detune;  /* 失谐，7 = 中心 */
}

static uint8_t g_voices[kFmVoiceCount][kDx7VoiceBytes];
static bool    g_built = false;

static void FillCommon(uint8_t *v, uint8_t algorithm, uint8_t feedback, const char *name,
                       uint8_t lfo_speed, uint8_t lfo_pmd, uint8_t lfo_amd) {
    memset(v, 0, kDx7VoiceBytes);
    v[kDx7Algorithm]   = algorithm;
    v[kDx7Feedback]    = feedback;
    v[kDx7OscKeySync]  = 0;
    v[kDx7LfoStart + 0] = lfo_speed;
    v[kDx7LfoStart + 1] = 0;
    v[kDx7LfoStart + 2] = lfo_pmd;
    v[kDx7LfoStart + 3] = lfo_amd;
    v[kDx7LfoStart + 4] = 0;
    v[kDx7LfoStart + 5] = 0;      /* 三角波 */
    v[kDx7LfoStart + 6] = 0;
    for (int i = 0; i < 4; ++i) { v[kDx7PegRates + i] = 99; v[kDx7PegLevels + i] = 50; }
    v[kDx7Transpose] = 24;
    for (int i = 0; i < 10 && name[i]; ++i) v[kDx7Name + i] = (uint8_t)name[i];
}

static void BuildVoices() {
    /* 算法 0 = DX7 算法 1（OP6→OP5→…→OP1 串联，OP1 是载波） */
    uint8_t *ep = g_voices[0];
    FillCommon(ep, 0, 5, "FM E.Piano", 35, 0, 0);
    Dx7SetOp(ep, 5, 99, 99, 99, 99, 99, 99, 99, 0, 99, 1, 7, 0, 0, 0);
    Dx7SetOp(ep, 4, 99, 85, 70, 70, 99, 75, 60, 0, 72, 1, 7, 2, 0, 0);
    Dx7SetOp(ep, 3, 99, 90, 80, 80, 99, 70,  0, 0, 45, 3, 7, 2, 0, 0);
    for (int op = 0; op < 3; ++op) Dx7SetOp(ep, op, 99, 99, 99, 99, 0, 0, 0, 0, 0, 1, 7, 0, 0, 0);

    uint8_t *bs = g_voices[1];
    FillCommon(bs, 0, 0, "FM Bass", 20, 0, 0);
    Dx7SetOp(bs, 5, 99, 92, 80, 75, 99, 88, 80, 0, 99, 1, 7, 0, 0, 0);
    Dx7SetOp(bs, 4, 99, 80, 60, 60, 99, 60,  0, 0, 82, 2, 6, 1, 0, 0);
    Dx7SetOp(bs, 3, 99, 70, 50, 50, 80, 40,  0, 0, 40, 1, 8, 1, 0, 0);
    for (int op = 0; op < 3; ++op) Dx7SetOp(bs, op, 99, 99, 99, 99, 0, 0, 0, 0, 0, 1, 7, 0, 0, 0);

    uint8_t *bl = g_voices[2];
    FillCommon(bl, 0, 3, "FM Bell", 45, 0, 0);
    Dx7SetOp(bl, 5, 99, 99, 99, 99, 99, 90, 60, 0, 99, 1, 7, 0, 0, 0);
    Dx7SetOp(bl, 4, 99, 95, 85, 80, 99, 60, 30, 0, 78, 7, 7, 0, 0, 0);   /* 非整数比 → 铃声 */
    Dx7SetOp(bl, 3, 99, 90, 80, 80, 99, 40,  0, 0, 55, 11, 7, 0, 0, 0);
    for (int op = 0; op < 3; ++op) Dx7SetOp(bl, op, 99, 99, 99, 99, 0, 0, 0, 0, 0, 1, 7, 0, 0, 0);

    g_built = true;
}

const uint8_t *FmVoice(int index) {
    if (!g_built) BuildVoices();
    if (index < 0 || index >= kFmVoiceCount) index = 0;
    return g_voices[index];
}

const char *FmVoiceName(int index) {
    if (!g_built) BuildVoices();
    static const char *names[kFmVoiceCount] = {"FM E.Piano", "FM Bass", "FM Bell"};
    if (index < 0 || index >= kFmVoiceCount) index = 0;
    return names[index];
}

}  // namespace ssynth
