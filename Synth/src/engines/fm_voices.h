/* 内置 DX7 音色（VCED 155 字节布局），自研示例音色（首次调用时构造）。 */
#ifndef SSYNTH_FM_VOICES_H
#define SSYNTH_FM_VOICES_H

#include <stdint.h>
#include "fm_dx7.h"

namespace ssynth {

constexpr int kFmVoiceCount = 3;

/* 返回指向 156 字节 VCED 数据的指针（静态存储，程序生命周期有效） */
const uint8_t *FmVoice(int index);
const char    *FmVoiceName(int index);

/* 填一个运算器块（op_idx 0=OP6 … 5=OP1），便于自研音色 */
void Dx7SetOp(uint8_t *voice, int op_idx,
              uint8_t r1, uint8_t r2, uint8_t r3, uint8_t r4,
              uint8_t l1, uint8_t l2, uint8_t l3, uint8_t l4,
              uint8_t out_level, uint8_t coarse, uint8_t detune,
              uint8_t kvs, uint8_t rs, uint8_t ams);

}  // namespace ssynth
#endif /* SSYNTH_FM_VOICES_H */
