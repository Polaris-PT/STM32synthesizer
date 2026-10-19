/* MIDI 1.0 字节流解析：running status、实时字节穿插、SysEx 丢弃、系统公共消息不污染 running status。
 *
 * 为什么要注意「系统公共消息」：0xF1(1 字节) 0xF2(2 字节) 0xF3(1 字节) 0xF6(0 字节) 0xF4/0xF5 未定义。
 * 老版本把所有 >= 0xF1 的消息都当 2 数据字节，收到 0xF1/0xF3 后会多吃掉后面一个字节，
 * 而且把 0xF1 当成 running status 留着 —— 之后一个数据字节就会被错解成一条消息。
 * 硬件上很少见，但 USB-MIDI 主机转发畸形数据、或把 System Common 混进 note 流时就会乱。 */
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

    /* 统计（排障用：gdb 看 ssynth::Engine::parser 即可） */
    uint32_t sysex_bytes    = 0;   /* 被丢弃的 SysEx 字节数 */
    uint32_t dropped_sysex  = 0;   /* 被丢弃的 SysEx 消息数 */
    uint32_t stray_bytes    = 0;   /* 没有状态字节却收到数据字节（流被截断/错位） */
    uint32_t dropped_common = 0;   /* 0xF4/0xF5/0xF6 这类无数据或未定义的系统消息 */

    void Reset() {
        status = 0; count = 0; expected = 0; in_sysex = 0;
        sysex_bytes = 0; dropped_sysex = 0; stray_bytes = 0; dropped_common = 0;
    }

    /* 系统公共消息的数据字节数；0 表示"没有数据字节"（含未定义的 0xF4/0xF5） */
    static inline uint8_t SysCommonLen(uint8_t b) {
        switch (b) {
            case 0xF1u: return 1u;   /* MTC Quarter Frame */
            case 0xF2u: return 2u;   /* Song Position Pointer */
            case 0xF3u: return 1u;   /* Song Select */
            default:    return 0u;   /* 0xF4/0xF5 未定义、0xF6 Tune Request 无数据 */
        }
    }

    /* 送入一个字节；返回 1 表示产出一条完整消息（st/d1/d2 有效）。
       实时字节（0xF8..0xFF）被忽略但不破坏状态；SysEx 被整体丢弃。 */
    inline int Feed(uint8_t b, uint8_t *st, uint8_t *d1, uint8_t *d2) {
        if (b == 0xF0u) {                        /* SysEx 开始：整体丢弃下面的内容 */
            in_sysex = 1; sysex_bytes = 0; status = 0; count = 0; expected = 0;
            return 0;
        }
        if (b == 0xF7u) {                        /* SysEx 结束 */
            if (in_sysex) { ++dropped_sysex; }
            in_sysex = 0; status = 0; count = 0; expected = 0;
            return 0;
        }
        if (b & 0x80u) {
            if (b >= 0xF8u) return 0;            /* realtime：任意位置出现，忽略 */
            if (in_sysex) { in_sysex = 0; ++dropped_sysex; }
            if ((b & 0xF0u) == 0xF0u) {          /* 系统公共消息：不建立 running status */
                status   = b;
                count    = 0;
                expected = SysCommonLen(b);
                if (expected == 0u) { ++dropped_common; status = 0; }
                return 0;
            }
            status   = b;
            count    = 0;
            expected = ((b & 0xF0u) == 0xC0u || (b & 0xF0u) == 0xD0u) ? 1u : 2u;
            return 0;
        }
        if (in_sysex) { ++sysex_bytes; return 0; }
        if (status == 0u) { ++stray_bytes; return 0; }
        if (count < 2u) data[count++] = (uint8_t)(b & 0x7Fu);
        if (count >= expected) {
            *st = status; *d1 = data[0]; *d2 = data[1];
            count = 0;
            if ((status & 0xF0u) == 0xF0u) status = 0;   /* 系统消息用一次就作废 */
            return 1;
        }
        return 0;
    }
};

}  // namespace ssynth
#endif /* SSYNTH_MIDI_PARSER_H */
