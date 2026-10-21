/* 阶段 3：QSPI flash 采样（内存映射只读访问）+ 采样库的枚举/导入。
 *
 * 布局（详见 docs/sample-library.md）：
 *   0x000000  header（32 B）           magic 'SLIB' / version / 计数 / 数据区偏移 / serial / crc
 *   0x000020  instrument[]（24 B 一条）name[16] + pc + zone_count + volume + mismatch_limit + first_zone
 *   0x000020+24N zone[]（32 B 一条）   offset/frames/rate/loop_start/loop_end/root/low/high/flags/interp/gain/crossfade
 *   0x004000  采样数据（s16 单声道 PCM，每条 4 字节对齐）
 *   0x7FF000  留给 QSPI 自检（App_QspiWriteTest 用，库不许占）
 *
 * 播放：zone->data = 0x90000000 + offset，零拷贝交给引擎（ROMpler）。
 * 导入：SD 卡 /synth/samples.bin（PC 用 tools/pack_samples.py 打包）→ 擦/写 QSPI。
 */
#include "synth_port_config.h"
#include "synth_port.h"
#include "qspi_flash.h"
#include "synth/synth.h"
#include "synth/sample.h"

#if SYNTH_PORT_SD_ENABLE
#include "ff.h"
#endif

#ifndef SYNTH_QSPI_MAP_BASE
#define SYNTH_QSPI_MAP_BASE 0x90000000u
#endif

#define SYNTH_LIB_MAGIC      0x42494C53u  /* 'SLIB'（小端） */
#define SYNTH_LIB_VERSION    2u            /* v2：instrument 的 reserved 字节改作 mismatch_limit */
#define SYNTH_LIB_DESC_BYTES 0x4000u      /* 目录区 16 KB，之后才是数据 */
#define SYNTH_LIB_DATA_BASE  SYNTH_LIB_DESC_BYTES
#define SYNTH_LIB_DATA_END   0x7FF000u    /* 给 QSPI 自检扇区让路 */
#define SYNTH_LIB_PATH       "0:/synth/samples.bin"

typedef struct {                          /* 32 B */
    uint32_t magic;
    uint16_t version;
    uint16_t hdr_bytes;
    uint32_t instr_count;
    uint32_t zone_count;
    uint32_t data_offset;
    uint32_t data_bytes;
    uint32_t serial;
    uint32_t crc;
    uint8_t  reserved[4];
} synth_lib_header_t;

typedef struct {                          /* 24 B */
    char     name[16];
    uint8_t  pc;                          /* 挂在哪个 Program Change（0xFF = 不挂） */
    uint8_t  zone_count;
    uint8_t  volume;                      /* 0..255 → 0..1 */
    uint8_t  mismatch_limit;              /* 允许的最大失配半音数（0 = 不限制）：zone 的键区会被
                                             夹到 root ± limit，免得一个采样被拉伸到不像样 */
    uint16_t first_zone;
    uint16_t reserved2;
} synth_lib_instr_t;

typedef struct {                          /* 32 B */
    uint32_t offset;                      /* 相对 0x90000000 的字节偏移 */
    uint32_t frames;
    uint32_t rate;
    uint32_t loop_start;
    uint32_t loop_end;                    /* 0 = 到采样末尾 */
    uint8_t  root;
    uint8_t  low;
    uint8_t  high;
    uint8_t  flags;                       /* bit0 = 循环 */
    uint8_t  interp;                      /* 0 = 线性，1 = 三次 */
    uint8_t  pad[3];
    uint16_t gain_q8;                     /* 0..512 → 0..2.0 */
    uint16_t crossfade;
} synth_lib_zone_t;

static synth_sample_instrument_t g_qspi_instrument;
static uint32_t s_lib_serial;             /* 最近一次成功读到/导入的库 serial */
static int32_t  s_import_state;           /* 0 = 没导过；1 = 成功；负数 = 错误码 */
static uint32_t s_import_bytes;           /* 已写入字节（进度） */
static uint32_t s_import_serials;         /* 导入次数 */

/* ---------------------------------------------------------------- 原有 API */

void synth_port_sample_qspi_reset(void) {
    __builtin_memset(&g_qspi_instrument, 0, sizeof(g_qspi_instrument));
    synth_set_sample_instrument(NULL);
}

/* 把一个 QSPI 中的采样区加进乐器（可多次调用组成多音区） */
int synth_port_sample_add_qspi(uint32_t byte_offset, uint32_t frames, uint32_t rate,
                               uint8_t root, uint8_t low, uint8_t high,
                               uint8_t loop, uint32_t loop_start, uint32_t loop_end) {
    if (frames < 2u) return -1;
    if (g_qspi_instrument.zone_count >= SYNTH_ROM_MAX_ZONES) return -2;

    synth_sample_zone_t *z = &g_qspi_instrument.zones[g_qspi_instrument.zone_count];
    __builtin_memset(z, 0, sizeof(*z));
    z->data       = (const int16_t *)(SYNTH_QSPI_MAP_BASE + byte_offset);
    z->frames     = frames;
    z->rate       = rate;
    z->root       = root;
    z->low        = low;
    z->high       = high;
    z->loop       = loop;
    z->loop_start = loop_start;
    z->loop_end   = loop_end;
    z->gain       = 1.0f;
    ++g_qspi_instrument.zone_count;
    g_qspi_instrument.volume = 0.8f;
    return 0;
}

void synth_port_sample_qspi_activate(const char *name) {
    if (g_qspi_instrument.zone_count == 0u) return;
    if (name != NULL) {
        unsigned i = 0;
        for (; i + 1u < sizeof(g_qspi_instrument.name) && name[i] != '\0'; ++i) g_qspi_instrument.name[i] = name[i];
        g_qspi_instrument.name[i] = '\0';
    }
    synth_set_sample_instrument(&g_qspi_instrument);
    synth_set_engine_type(2u);
}

/* ------------------------------------------------------------ 库：目录访问 */

static const synth_lib_header_t *lib_header(void) {
    return (const synth_lib_header_t *)(SYNTH_QSPI_MAP_BASE);
}
static const synth_lib_instr_t *lib_instrs(void) {
    return (const synth_lib_instr_t *)(SYNTH_QSPI_MAP_BASE + sizeof(synth_lib_header_t));
}
static const synth_lib_zone_t *lib_zones(void) {
    const synth_lib_header_t *h = lib_header();
    return (const synth_lib_zone_t *)(SYNTH_QSPI_MAP_BASE + sizeof(synth_lib_header_t) +
                                      (uint32_t)h->instr_count * sizeof(synth_lib_instr_t));
}

/* 库是否有效（magic/version/数据区在合理范围） */
int synth_port_lib_probe(void) {
    const synth_lib_header_t *h = lib_header();
    if (h->magic != SYNTH_LIB_MAGIC) return 0;
    if (h->version != SYNTH_LIB_VERSION) return 0;
    if (h->data_offset < SYNTH_LIB_DESC_BYTES) return 0;
    if (h->data_offset + h->data_bytes > SYNTH_LIB_DATA_END) return 0;
    if (h->instr_count == 0u || h->instr_count > 128u) return 0;
    if (h->zone_count == 0u || h->zone_count > 512u) return 0;
    return 1;
}

uint32_t synth_port_lib_serial(void) { return synth_port_lib_probe() ? lib_header()->serial : 0u; }
uint32_t synth_port_lib_count(void)  { return synth_port_lib_probe() ? lib_header()->instr_count : 0u; }

/* 第 i 件乐器的名字/PC/音区数；返回 0 = 成功 */
int synth_port_lib_info(uint32_t i, char *name16, uint8_t *pc, uint8_t *zone_count) {
    if (!synth_port_lib_probe() || i >= lib_header()->instr_count) return -1;
    const synth_lib_instr_t *ins = &lib_instrs()[i];
    if (name16 != NULL) {
        for (unsigned k = 0; k < 16u; ++k) { name16[k] = ins->name[k]; if (name16[k] == '\0') break; }
        name16[15] = '\0';
    }
    if (pc != NULL) *pc = ins->pc;
    if (zone_count != NULL) *zone_count = ins->zone_count;
    return 0;
}

/* 某件乐器允许的最大失配半音数（0 = 不限制；越界返回 0） */
uint8_t synth_port_lib_mismatch_limit(uint32_t i) {
    if (!synth_port_lib_probe() || i >= lib_header()->instr_count) return 0u;
    return lib_instrs()[i].mismatch_limit;
}

/* 按 Program Change 号把库里的乐器挂到引擎（零拷贝）；返回 0 = 成功、-1 = 库里没有 */
int synth_port_lib_load_pc(uint8_t pc, char *name_out) {
    if (!synth_port_lib_probe()) return -1;
    const synth_lib_header_t *h = lib_header();
    const synth_lib_instr_t *ins = lib_instrs();
    const synth_lib_zone_t *zones = lib_zones();

    for (uint32_t i = 0; i < h->instr_count; ++i) {
        if (ins[i].pc != pc) continue;
        if (ins[i].zone_count == 0u) return -1;
        if ((uint32_t)ins[i].first_zone + ins[i].zone_count > h->zone_count) return -1;

        synth_port_sample_qspi_reset();
        /* mismatch_limit > 0 时把每个 zone 的键区夹到 root ± limit：包工具已经算过一遍，
           这里再夹一次是防御 —— 采样被拉得太远会明显失真（而且很费 CPU）。 */
        const uint8_t mlim = ins[i].mismatch_limit;
        for (uint8_t k = 0u; k < ins[i].zone_count; ++k) {
            const synth_lib_zone_t *z = &zones[ins[i].first_zone + k];
            if (z->offset < SYNTH_LIB_DATA_BASE || (uint64_t)z->offset + (uint64_t)z->frames * 2u > SYNTH_LIB_DATA_END) continue;
            uint8_t lo = z->low, hi = z->high;
            if (mlim > 0u) {
                const uint8_t r = z->root;
                const uint8_t lmin = (r > mlim) ? (uint8_t)(r - mlim) : 0u;
                const uint8_t lmax = ((uint16_t)r + mlim > 127u) ? 127u : (uint8_t)(r + mlim);
                if (lo < lmin) lo = lmin;
                if (hi > lmax) hi = lmax;
                if (lo > hi) continue;       /* 夹没了：这个 zone 对任何键都不该用 */
            }
            if (synth_port_sample_add_qspi(z->offset, z->frames, z->rate, z->root, lo, hi,
                                           (uint8_t)(z->flags & 1u), z->loop_start, z->loop_end) != 0) continue;
            synth_sample_zone_t *sz = &g_qspi_instrument.zones[g_qspi_instrument.zone_count - 1u];
            sz->gain      = (float)z->gain_q8 / 256.0f;
            sz->crossfade = z->crossfade;
            sz->interp    = (z->interp != 0u) ? 1u : 0u;
        }
        if (g_qspi_instrument.zone_count == 0u) return -1;
        g_qspi_instrument.volume = (float)ins[i].volume / 255.0f;
        if (name_out != NULL) {
            for (unsigned k = 0; k < 16u; ++k) { name_out[k] = ins[i].name[k]; if (name_out[k] == '\0') break; }
            name_out[15] = '\0';
        }
        synth_port_sample_qspi_activate(ins[i].name);
        return 0;
    }
    return -1;
}

/* 库里有没有挂在某个 PC 上的乐器（给 patch_service 判优先级用） */
int synth_port_lib_has_pc(uint8_t pc) {
    if (!synth_port_lib_probe()) return 0;
    const synth_lib_header_t *h = lib_header();
    const synth_lib_instr_t *ins = lib_instrs();
    for (uint32_t i = 0; i < h->instr_count; ++i) {
        if (ins[i].pc == pc) return 1;
    }
    return 0;
}

/* 主循环 / 开机调用：主音色号变成库里挂着的 PC 时零拷贝挂载库乐器。
   QSPI 库优先于卡上的 /synth/NNN.ins|.syn —— 想用卡上版本就把库里那个 PC 换号。
   返回 1 = 本次挂载了库乐器、0 = 没变化/库里没有。 */
int synth_port_lib_service(void) {
    static int16_t s_last_pc = -2;
    const int16_t cur = (int16_t)synth_get_patch_index();
    if (cur == s_last_pc) return 0;
    s_last_pc = cur;
    if (cur < 0) return 0;
    return (synth_port_lib_load_pc((uint8_t)cur, NULL) == 0) ? 1 : 0;
}

/* -------------------------------------------------------------- 库：导入 */
/* 进度/结果（给 status.ps1 看） */
int32_t  synth_port_lib_import_state(void)    { return s_import_state; }
uint32_t synth_port_lib_import_progress(void) { return s_import_bytes; }
uint32_t synth_port_lib_import_count(void)    { return s_import_serials; }

/* 把一段 RAM 数据按 256 字节页写进 QSPI（调用前扇区已擦好） */
static int program_range(uint32_t offset, const uint8_t *src, uint32_t len) {
    for (uint32_t done = 0u; done < len; ) {
        uint32_t chunk = len - done;
        const uint32_t room = SYNTH_QSPI_PAGE_BYTES - ((offset + done) % SYNTH_QSPI_PAGE_BYTES);
        if (chunk > room) chunk = room;
        if (synth_port_qspi_page_program(offset + done, src + done, chunk) != 0) return -1;
        done += chunk;
        s_import_bytes += chunk;
    }
    return 0;
}

/* 擦掉 [0, end) 覆盖的扇区 */
static int erase_range(uint32_t end) {
    for (uint32_t off = 0u; off < end; off += SYNTH_QSPI_SECTOR_BYTES) {
        if (synth_port_qspi_erase_sector(off) != 0) return -1;
    }
    return 0;
}

#if SYNTH_PORT_SD_ENABLE
/* 从 SD 卡的 /synth/samples.bin 导入。返回 0 = 已完成（含"已经是最新，无需导入"）、
   负数 = 失败（-1 打不开、-2 头非法、-3 擦除失败、-4 写入失败、-5 校验失败）。 */
int synth_port_lib_import_from_card(void) {
    FIL f;
    UINT got = 0u;
    synth_lib_header_t h;
    static uint8_t buf[SYNTH_QSPI_PAGE_BYTES];

    s_import_state = -1;
    s_import_bytes = 0u;

    if (f_open(&f, SYNTH_LIB_PATH, FA_READ) != FR_OK) return -1;
    if (f_read(&f, &h, sizeof(h), &got) != FR_OK || got != sizeof(h)) { f_close(&f); return -2; }
    if (h.magic != SYNTH_LIB_MAGIC || h.version != SYNTH_LIB_VERSION) { f_close(&f); return -2; }
    if (h.data_offset < SYNTH_LIB_DESC_BYTES || h.data_offset > SYNTH_LIB_DATA_END) { f_close(&f); return -2; }
    if ((uint64_t)h.data_offset + h.data_bytes > SYNTH_LIB_DATA_END) { f_close(&f); return -2; }

    /* 已经是同一份库（serial 相同且目录有效）→ 什么都不做 */
    if (synth_port_lib_probe() && lib_header()->serial == h.serial) {
        f_close(&f);
        s_import_state = 1;
        return 0;
    }

    const uint32_t total = h.data_offset + h.data_bytes;
    if (erase_range(total) != 0) { f_close(&f); s_import_state = -3; return -3; }

    /* 从头流式写：头/目录/数据（头最后单独重写一遍，保证"中途断电不会看起来有效"） */
    if (f_lseek(&f, 0u) != FR_OK) { f_close(&f); s_import_state = -4; return -4; }
    while (s_import_bytes < total) {
        UINT want = (UINT)((total - s_import_bytes) < sizeof(buf) ? (total - s_import_bytes) : sizeof(buf));
        if (f_read(&f, buf, want, &got) != FR_OK || got == 0u) { f_close(&f); s_import_state = -4; return -4; }
        if (program_range(s_import_bytes, buf, got) != 0) { f_close(&f); s_import_state = -4; return -4; }
    }
    f_close(&f);

    /* 校验：重新进映射，比一下头和 serial */
    (void)synth_port_qspi_enter_mmap();
    synth_port_qspi_cache_invalidate();
    if (!synth_port_lib_probe() || lib_header()->serial != h.serial) {
        s_import_state = -5;
        return -5;
    }
    s_lib_serial = h.serial;
    s_import_state = 1;
    ++s_import_serials;
    return 0;
}
#endif /* SYNTH_PORT_SD_ENABLE */

/* ------------------------------------------------------------ 库：自检 */
#if SYNTH_PORT_LIB_SELFTEST_ON_BOOT
/* 造一个内存里的小库（1 件乐器、1 个音区、512 帧循环正弦）→ 走真实导入路径写进 QSPI →
   再枚举/挂载一次。用来在没有 PC 打包文件时验证「擦/写/目录/挂载」整条链路。
   注意：会覆盖 QSPI 上的真库（测试完请重新导入真库）。
   只有在 SYNTH_PORT_LIB_SELFTEST_ON_BOOT = 1 时才编译进来（省 flash + 省 20 KB RAM）。 */
static uint8_t s_selftest_image[SYNTH_LIB_DESC_BYTES + 4096u];   /* 目录区 + 2048 帧 PCM */

int synth_port_lib_selftest(void) {
    synth_lib_header_t h;
    synth_lib_instr_t ins;
    synth_lib_zone_t   z;
    const uint32_t frames = 2048u;

    __builtin_memset(s_selftest_image, 0, sizeof(s_selftest_image));
    __builtin_memset(&h, 0, sizeof(h));
    __builtin_memset(&ins, 0, sizeof(ins));
    __builtin_memset(&z, 0, sizeof(z));

    h.magic = SYNTH_LIB_MAGIC;
    h.version = SYNTH_LIB_VERSION;
    h.hdr_bytes = (uint16_t)sizeof(h);
    h.instr_count = 1u;
    h.zone_count = 1u;
    h.data_offset = SYNTH_LIB_DATA_BASE;
    h.data_bytes = frames * 2u;
    h.serial = 0x5E1F7E57u;              /* 'SElfTEST' 风格：固定值，每次自检都同一份 */

    ins.name[0] = 'S'; ins.name[1] = 'e'; ins.name[2] = 'l'; ins.name[3] = 'f';
    ins.name[4] = 'T'; ins.name[5] = 'e'; ins.name[6] = 's'; ins.name[7] = 't';
    ins.pc = 120u;
    ins.zone_count = 1u;
    ins.volume = 200u;
    ins.first_zone = 0u;

    z.offset = SYNTH_LIB_DATA_BASE;
    z.frames = frames;
    z.rate = 48000u;
    z.loop_start = frames - 512u;        /* 循环最后 512 帧（48 kHz / 512 = 93.75 Hz 的整周期） */
    z.loop_end = 0u;
    z.root = 69u;                        /* A4 */
    z.low = 0u;
    z.high = 127u;
    z.flags = 1u;                        /* 循环 */
    z.interp = 1u;
    z.gain_q8 = 256u;
    z.crossfade = 128u;

    __builtin_memcpy(s_selftest_image, &h, sizeof(h));
    __builtin_memcpy(s_selftest_image + sizeof(h), &ins, sizeof(ins));
    __builtin_memcpy(s_selftest_image + sizeof(h) + sizeof(ins), &z, sizeof(z));

    /* 512 帧一个整周期的正弦，重复 4 次 */
    int16_t *pcm = (int16_t *)(void *)(s_selftest_image + SYNTH_LIB_DATA_BASE);
    for (uint32_t n = 0; n < frames; ++n) {
        const uint32_t ph = n % 512u;
        /* sin(2π·ph/512) × 12000，用查表太小题大做：这里用 8 点折线近似 */
        static const int16_t tri[9] = {0, 8485, 12000, 8485, 0, -8485, -12000, -8485, 0};
        const uint32_t seg = ph / 64u;             /* 64 点一段 */
        const uint32_t frac = ph % 64u;
        const int32_t a = tri[seg], b = tri[seg + 1u];
        pcm[n] = (int16_t)(a + (b - a) * (int32_t)frac / 64);
    }

    s_import_state = -1;
    s_import_bytes = 0u;
    if (erase_range(SYNTH_LIB_DATA_BASE + frames * 2u) != 0) { s_import_state = -3; return -3; }
    if (program_range(0u, s_selftest_image, SYNTH_LIB_DATA_BASE + frames * 2u) != 0) { s_import_state = -4; return -4; }
    (void)synth_port_qspi_enter_mmap();
    synth_port_qspi_cache_invalidate();
    if (!synth_port_lib_probe() || lib_header()->serial != h.serial) { s_import_state = -5; return -5; }
    s_lib_serial = h.serial;
    s_import_state = 1;
    ++s_import_serials;
    return (synth_port_lib_load_pc(120u, NULL) == 0) ? 0 : -6;
}
#else  /* !SYNTH_PORT_LIB_SELFTEST_ON_BOOT */
int synth_port_lib_selftest(void) { return -7; }   /* 未编译进来（省 flash + 20 KB RAM） */
#endif
