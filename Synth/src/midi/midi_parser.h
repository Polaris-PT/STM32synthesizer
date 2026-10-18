/* MIDI 1.0 字节流解析：running status、实时字节穿插、SysEx 丢弃。 */
#ifndef SSYNTH_MIDI_PARSER_H
#define SSYNTH_MIDI_PARSER_H

#include <stdint.h>

namespace ssynth {

struct MidiParser {
    uint8_t  status   = 0;
    uint8_t  data[2]  = {0, 0};
    uint8_t  count    = 0;
    uint8_t  expected = 0;
    uint8_t  in_sysex = 0;
    uint32_t sysex_bytes = 0;     /* 统计用 */
    uint32_t dropped_sysex = 0;

    void Reset() {
        status = 0; count = 0; expected = 0; in_sysex = 0;
        sysex_bytes = 0; dropped_sysex = 0;
    }

    /* 送入一个字节；返回 1 表示产出一条完整消息（st/d1/d2 有效）。
       实时字节（0xF8..0xFF）被忽略但不破坏状态；SysEx 被整体丢弃。 */
    inline int Feed(uint8_t b, uint8_t *st, uint8_t *d1, uint8_t *d2) {
        if (b & 0x80u) {
            if (b >= 0xF8u) return 0;                  /* realtime：任意位置出现，忽略 */
            if (b == 0xF0u) { in_sysex = 1; sysex_bytes = 0; status = 0; count = 0; return 0; }
            if (b == 0xF7u) { if (in_sysex) { ++dropped_sysex; } in_sysex = 0; return 0; }
            if (in_sysex) { in_sysex = 0; ++dropped_sysex; }
            status   = b;
            count    = 0;
            expected = ((b & 0xF0u) == 0xC0u || (b & 0xF0u) == 0xD0u) ? 1u : 2u;
            return 0;
        }
        if (in_sysex) { ++sysex_bytes; return 0; }
        if (status == 0) return 0;
        if (count < 2u) data[count++] = (uint8_t)(b & 0x7Fu);
        if (count >= expected) {
            *st = status; *d1 = data[0]; *d2 = data[1];
            count = 0;
            return 1;
        }
        return 0;
    }
};

}  // namespace ssynth
#endif /* SSYNTH_MIDI_PARSER_H */
