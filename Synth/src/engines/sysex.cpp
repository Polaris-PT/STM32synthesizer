#include "sysex.h"
#include <string.h>

namespace ssynth {

/* VCED 运算器块 21 字节；压缩块 17 字节 */
static const int kOpVced = 21;
static const int kOpPack = 17;

void UnpackVmemVoice(const uint8_t packed[128], uint8_t vced[kDx7VoiceBytes]) {
    memset(vced, 0, kDx7VoiceBytes);
    for (int op = 0; op < 6; ++op) {
        const uint8_t *p = packed + op * kOpPack;
        uint8_t *v = vced + op * kOpVced;
        v[0] = p[0]; v[1] = p[1]; v[2] = p[2]; v[3] = p[3];        /* EG R1-R4 */
        v[4] = p[4]; v[5] = p[5]; v[6] = p[6]; v[7] = p[7];        /* EG L1-L4 */
        v[8]  = p[8];                                              /* BP */
        v[9]  = p[9];                                              /* LD */
        v[10] = p[10];                                             /* RD */
        v[11] = (uint8_t)(p[11] & 0x03);                           /* LC */
        v[12] = (uint8_t)((p[11] >> 2) & 0x03);                    /* RC */
        v[13] = (uint8_t)(p[12] & 0x07);                           /* RS */
        v[14] = (uint8_t)(p[13] & 0x03);                           /* AMS */
        v[15] = (uint8_t)((p[13] >> 2) & 0x07);                    /* KVS */
        v[16] = p[14];                                             /* OL */
        v[17] = (uint8_t)(p[15] & 0x01);                           /* OSC MODE */
        v[18] = (uint8_t)((p[15] >> 1) & 0x1F);                    /* FREQ COARSE */
        v[19] = p[16];                                             /* FREQ FINE */
        v[20] = (uint8_t)((p[12] >> 3) & 0x0F);                    /* DETUNE */
    }
    for (int i = 0; i < 4; ++i) {
        vced[126 + i] = packed[102 + i];                           /* PEG R1-R4 */
        vced[130 + i] = packed[106 + i];                           /* PEG L1-L4 */
    }
    vced[134] = (uint8_t)(packed[110] & 0x1F);                     /* ALGORITHM */
    vced[135] = (uint8_t)(packed[111] & 0x07);                     /* FEEDBACK */
    vced[136] = (uint8_t)((packed[111] >> 3) & 0x01);              /* OSC KEY SYNC */
    vced[137] = packed[112];                                       /* LFO SPEED */
    vced[138] = packed[113];                                       /* LFO DELAY */
    vced[139] = packed[114];                                       /* LFO PITCH MOD DEPTH */
    vced[140] = packed[115];                                       /* LFO AMP MOD DEPTH */
    vced[141] = (uint8_t)(packed[116] & 0x01);                     /* LFO SYNC */
    vced[142] = (uint8_t)((packed[116] >> 1) & 0x07);              /* LFO WAVEFORM */
    vced[143] = (uint8_t)((packed[116] >> 4) & 0x07);              /* LFO PITCH MOD SENS */
    vced[144] = packed[117];                                       /* TRANSPOSE */
    for (int i = 0; i < 10; ++i) vced[145 + i] = packed[118 + i];  /* NAME */
}

void PackVmemVoice(const uint8_t vced[kDx7VoiceBytes], uint8_t packed[128]) {
    memset(packed, 0, 128);
    for (int op = 0; op < 6; ++op) {
        const uint8_t *v = vced + op * kOpVced;
        uint8_t *p = packed + op * kOpPack;
        p[0] = v[0]; p[1] = v[1]; p[2] = v[2]; p[3] = v[3];
        p[4] = v[4]; p[5] = v[5]; p[6] = v[6]; p[7] = v[7];
        p[8]  = v[8];
        p[9]  = v[9];
        p[10] = v[10];
        p[11] = (uint8_t)((v[11] & 0x03) | ((v[12] & 0x03) << 2));
        p[12] = (uint8_t)((v[13] & 0x07) | ((v[20] & 0x0F) << 3));
        p[13] = (uint8_t)((v[14] & 0x03) | ((v[15] & 0x07) << 2));
        p[14] = v[16];
        p[15] = (uint8_t)((v[17] & 0x01) | ((v[18] & 0x1F) << 1));
        p[16] = v[19];
    }
    for (int i = 0; i < 4; ++i) { packed[102 + i] = vced[126 + i]; packed[106 + i] = vced[130 + i]; }
    packed[110] = (uint8_t)(vced[134] & 0x1F);
    packed[111] = (uint8_t)((vced[135] & 0x07) | ((vced[136] & 0x01) << 3));
    packed[112] = vced[137];
    packed[113] = vced[138];
    packed[114] = vced[139];
    packed[115] = vced[140];
    packed[116] = (uint8_t)((vced[141] & 0x01) | ((vced[142] & 0x07) << 1) | ((vced[143] & 0x07) << 4));
    packed[117] = vced[144];
    for (int i = 0; i < 10; ++i) packed[118 + i] = vced[145 + i];
}

static bool IsDx7Header(const uint8_t *d, uint32_t len, uint32_t i, uint8_t *format) {
    if (i + 6u > len) return false;
    if (d[i] != 0xF0u || d[i + 1] != 0x43u) return false;
    *format = d[i + 3];
    return true;
}

int SysexToVoices(const uint8_t *data, uint32_t len, uint8_t *out, int max_out) {
    if (data == nullptr || out == nullptr || max_out <= 0) return kSysexNoData;

    /* 裸 VCED */
    if ((len == 155u || len == 156u) && (data[0] != 0xF0u)) {
        memcpy(out, data, 155u);
        out[155] = 0;
        return 1;
    }

    int written = 0;
    for (uint32_t i = 0; i + 6u <= len; ++i) {
        uint8_t format = 0;
        if (!IsDx7Header(data, len, i, &format)) continue;

        if (format == 0x00u) {
            /* 单音色非压缩：F0 43 ch 00 01 1B <155> chk F7 */
            if (i + 6u + 155u > len) return kSysexBadLength;
            if (data[i + 4] != 0x01u || data[i + 5] != 0x1Bu) return kSysexBadLength;
            if (written >= max_out) break;
            uint8_t *dst = out + (size_t)written * kDx7VoiceBytes;
            memcpy(dst, data + i + 6u, 155u);
            dst[155] = 0;
            ++written;
            i += 6u + 155u + 1u;                     /* 跳过数据与校验和 */
            continue;
        }
        if (format == 0x09u) {
            /* 32 音色压缩 bank：F0 43 ch 09 20 00 <4096> chk F7 */
            if (i + 6u + 4096u > len) return kSysexBadLength;
            if (data[i + 4] != 0x20u || data[i + 5] != 0x00u) return kSysexBadLength;
            for (int v = 0; v < 32; ++v) {
                if (written >= max_out) break;
                UnpackVmemVoice(data + i + 6u + (uint32_t)v * 128u,
                                out + (size_t)written * kDx7VoiceBytes);
                ++written;
            }
            i += 6u + 4096u + 1u;
            continue;
        }
        /* 其它格式（功能参数/ACED 等）跳过 */
        ++i;
    }
    return written > 0 ? written : kSysexNoData;
}

}  // namespace ssynth
