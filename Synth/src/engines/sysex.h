/* DX7 .syx 解析：非压缩单音色 dump（VCED, 163B）与压缩 bank dump（VMEM, 4104B）。
   位宽表依据 Yamaha DX7 MIDI Data Format Sheet（Bulk Dump Packed Format，见 docs/research/dxsyx/doc/dx7-sysex-format.txt）。 */
#ifndef SSYNTH_SYSEX_H
#define SSYNTH_SYSEX_H

#include <stdint.h>
#include "fm_dx7.h"

namespace ssynth {

enum SysexResult {
    kSysexOk = 0,
    kSysexNoData = -1,        /* 没找到可解析的 DX7 dump */
    kSysexBadLength = -2,
    kSysexUnsupported = -3,   /* 其它格式（如 ACED/功能参数） */
};

/* 从 data[0..len) 中提取 VCED 音色：
   - 支持 32 音色压缩 bank（F0 43 ch 09 20 00 …）
   - 支持单音色非压缩 dump（F0 43 ch 00 01 1B …）
   - 支持裸 155/156 字节 VCED
   out 为 max_out 个 156 字节槽位；返回写入的音色数（>=0，0 表示无数据）。 */
int SysexToVoices(const uint8_t *data, uint32_t len, uint8_t *out, int max_out);

/* 压缩 128 字节 → VCED 156 字节（单个音色） */
void UnpackVmemVoice(const uint8_t packed[128], uint8_t vced[kDx7VoiceBytes]);

/* 供测试/工具使用：VCED → 压缩 128 字节 */
void PackVmemVoice(const uint8_t vced[kDx7VoiceBytes], uint8_t packed[128]);

}  // namespace ssynth
#endif /* SSYNTH_SYSEX_H */
