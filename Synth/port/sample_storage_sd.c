/* 阶段 3：从 SD 卡载入 16bit PCM WAV 采样到 RAM 池，构造 ROMpler 乐器。
   采样一次性读入 RAM（不做实时流式），因此播放路径是纯 RAM 读取。
   仅在主循环调用（FATFS 不可在音频中断中使用）。 */
#include "synth_port_config.h"
#include "synth_port.h"
#include "synth/synth.h"

#include "ff.h"
#include "../src/app/instrument_text.h"
#include <string.h>

/* 采样池：放 AXI SRAM（.ram_section），512KB 供 ROMpler 使用 */
#ifndef SYNTH_SAMPLE_POOL_BYTES
#define SYNTH_SAMPLE_POOL_BYTES (512u * 1024u)
#endif

static int16_t SYNTH_PORT_AUDIO_RAM g_sample_pool[SYNTH_SAMPLE_POOL_BYTES / 2u];
static uint32_t g_pool_used = 0u;

static FIL g_wav_file;
static synth_sample_instrument_t g_instrument;

void synth_port_sample_pool_reset(void) {
    g_pool_used = 0u;
    memset(&g_instrument, 0, sizeof(g_instrument));
    synth_set_sample_instrument(NULL);
}

uint32_t synth_port_sample_pool_used(void) { return g_pool_used; }
uint32_t synth_port_sample_pool_size(void) { return SYNTH_SAMPLE_POOL_BYTES; }

static uint32_t rd_u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

/* 跳过 n 字节（按 FATFS 语义） */
static void skip(FIL *f, uint32_t n) { f_lseek(f, f_tell(f) + n); }

/* 只把 WAV 数据读进池（不建音区），供 .ins 清单的 loader 回调使用 */
static int load_wav_to_pool(const char *path, const int16_t **out_data, uint32_t *out_frames, uint32_t *out_rate);

/* .ins 清单的采样载入回调 */
static int sd_sample_loader(void *ctx, const char *path,
                            const int16_t **out_data, uint32_t *out_frames, uint32_t *out_rate) {
    (void)ctx;
    return load_wav_to_pool(path, out_data, out_frames, out_rate);
}

/* 载入 .ins 乐器清单：解析多音区 → 交给引擎并切到 ROMpler。返回音区数，负数错误。 */
int synth_port_instrument_load(const char *path) {
    static char text[2048];
    if (f_open(&g_wav_file, path, FA_READ) != FR_OK) return -1;
    UINT got = 0;
    const FRESULT fr = f_read(&g_wav_file, text, sizeof(text) - 1u, &got);
    f_close(&g_wav_file);
    if (fr != FR_OK) return -2;
    text[got < sizeof(text) ? got : sizeof(text) - 1u] = '\0';

    const int zones = instrument_from_text(text, &g_instrument, sd_sample_loader, NULL);
    if (zones < 0) return zones;
    synth_port_sample_activate();
    return zones;
}

/* 载入 16bit PCM WAV 到池中；成功返回帧数，失败返回负数。
   立体声会下混为单声道。loop_end == 0 表示到末尾。 */
int synth_port_sample_load_wav(const char *path, uint8_t root, uint8_t low, uint8_t high,
                               uint8_t loop, uint32_t loop_start, uint32_t loop_end) {
    if (f_open(&g_wav_file, path, FA_READ) != FR_OK) return -1;

    uint8_t hdr[12];
    UINT got = 0;
    if (f_read(&g_wav_file, hdr, 12u, &got) != FR_OK || got != 12u) { f_close(&g_wav_file); return -2; }
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) { f_close(&g_wav_file); return -3; }

    uint16_t channels = 0, bits = 0;
    uint32_t rate = 0;
    uint32_t data_bytes = 0;
    int      have_fmt = 0;

    /* 遍历子块 */
    for (;;) {
        uint8_t ch[8];
        if (f_read(&g_wav_file, ch, 8u, &got) != FR_OK || got != 8u) break;
        const uint32_t size = rd_u32(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (size < 16u || f_read(&g_wav_file, fmt, 16u, &got) != FR_OK || got != 16u) { f_close(&g_wav_file); return -4; }
            if (rd_u16(fmt) != 1u) { f_close(&g_wav_file); return -5; }       /* 仅 PCM */
            channels = rd_u16(fmt + 2);
            rate     = rd_u32(fmt + 4);
            bits     = rd_u16(fmt + 14);
            have_fmt = 1;
            if (size > 16u) skip(&g_wav_file, size - 16u);
        } else if (memcmp(ch, "data", 4) == 0) {
            data_bytes = size;
            break;
        } else {
            skip(&g_wav_file, size + (size & 1u));
        }
    }
    if (!have_fmt || data_bytes == 0u) { f_close(&g_wav_file); return -6; }
    if (bits != 16u || channels == 0u || channels > 2u) { f_close(&g_wav_file); return -7; }

    const uint32_t frames = data_bytes / (2u * channels);
    const uint32_t bytes_needed = frames * 2u;
    if (g_pool_used + bytes_needed > SYNTH_SAMPLE_POOL_BYTES) { f_close(&g_wav_file); return -8; }
    if (g_instrument.zone_count >= SYNTH_ROM_MAX_ZONES) { f_close(&g_wav_file); return -9; }

    int16_t *dst = &g_sample_pool[g_pool_used / 2u];
    if (channels == 1u) {
        if (f_read(&g_wav_file, dst, bytes_needed, &got) != FR_OK || got != bytes_needed) { f_close(&g_wav_file); return -10; }
    } else {
        static int16_t tmp[256];
        uint32_t left = frames;
        int16_t *out = dst;
        while (left > 0u) {
            const uint32_t n = (left > 128u) ? 128u : left;
            const UINT want = (UINT)(n * 4u);
            if (f_read(&g_wav_file, tmp, want, &got) != FR_OK || got != want) { f_close(&g_wav_file); return -11; }
            for (uint32_t i = 0; i < n; ++i) {
                out[i] = (int16_t)(((int32_t)tmp[2u * i] + (int32_t)tmp[2u * i + 1u]) / 2);  /* 下混 */
            }
            out += n;
            left -= n;
        }
    }
    f_close(&g_wav_file);

    synth_sample_zone_t *z = &g_instrument.zones[g_instrument.zone_count];
    memset(z, 0, sizeof(*z));
    z->data       = dst;
    z->frames     = frames;
    z->rate       = rate;
    z->root       = root;
    z->low        = low;
    z->high       = high;
    z->loop       = loop;
    z->loop_start = loop_start;
    z->loop_end   = loop_end;
    z->gain       = 1.0f;
    ++g_instrument.zone_count;
    g_instrument.volume = 0.8f;
    strncpy(g_instrument.name, "SD Sample", sizeof(g_instrument.name) - 1u);
    g_pool_used += bytes_needed;

    return (int)frames;
}

/* 只把 WAV 读进池：实现放在 synth_port_sample_load_wav 之后（复用其解析） */
static int load_wav_to_pool(const char *path, const int16_t **out_data, uint32_t *out_frames, uint32_t *out_rate) {
    const uint32_t used_before = g_pool_used;
    const uint8_t zones_before = g_instrument.zone_count;
    const int frames = synth_port_sample_load_wav(path, 60, 0, 127, 0, 0, 0);
    if (frames <= 0) return -1;
    const synth_sample_zone_t *z = &g_instrument.zones[zones_before];
    *out_data   = z->data;
    *out_frames = z->frames;
    *out_rate   = z->rate;
    /* 回退临时音区，最终的音区由 .ins 解析结果写入 */
    g_instrument.zone_count = zones_before;
    (void)used_before;
    return 0;
}

/* 把已载入的音区交给引擎并切到 ROMpler */
void synth_port_sample_activate(void) {
    if (g_instrument.zone_count == 0u) return;
    synth_set_sample_instrument(&g_instrument);
    synth_set_engine_type(2u);
}
