/* SD 卡音色存储：/synth/NNN.syn（0:/synth）。
   仅在主循环调用（FATFS 不可重入、不可在音频中断里使用）。
   CubeMX 里还没启用 FatFs 时（SYNTH_PORT_SD_ENABLE=0）退化为空实现，保证工程可编译。 */
#include "synth_port_config.h"
#include "synth_port.h"      /* synth_port_instrument_load() 的声明（实现在 sample_storage_sd.c） */
#include "synth/synth.h"

#if SYNTH_PORT_SD_ENABLE

#include "../src/app/patch_text.h"
#include "ff.h"
#include "stm32h7xx_hal.h"   /* HAL_GetTick()：状态持久化的静默窗口计时 */
#include "module.h"          /* RAM overlay 模块头布局（/synth/ui.bin 导入时用） */
#include "qspi_flash.h"      /* 模块镜像写进 QSPI 的原语 */
#include <string.h>
#include <stdio.h>

/* FATFS 对象（含 512B 的 fs->win 扇区窗口，f_read/f_write 的数据都从这里过）必须在
   AXI SRAM：SDMMC 走芯片内部 IDMA，HAL 把调用方的指针直接写进 SDMMC_IDMABASE0
   （Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_sd.c:1288），而 IDMA 挂在 AXI 上、
   **访问不到 DTCM**（0x20000000）—— 链接脚本默认把 .bss 放 DTCM，所以这里显式指定段。
   注意：这一段仍是 D-Cache 可缓存的（默认内存映射），所以启用 SD（SYNTH_PORT_SD_ENABLE=1）
   之前还要处理 cache 一致性：要么给 FATFS 对象单独一个非缓存 MPU region，要么在
   diskio 读写前后做 SCB_CleanInvalidateDCache_by_Addr。见 docs/phase2-review.md 的待办。 */
static FATFS SYNTH_PORT_AUDIO_RAM g_fs;
static int         g_mounted = 0;
static FIL         g_file;
static char        g_text[SYNTH_PORT_PATCH_MAXLEN];
static synth_patch_t g_loaded_patch;
static int16_t     g_last_request = -1;

int synth_port_sd_mount(void) {
    const FRESULT fr = f_mount(&g_fs, "0:", 1);
    g_mounted = (fr == FR_OK) ? 1 : 0;
    /* 失败时把 FatFs 的 FRESULT 原样返回（1 = FR_DISK_ERR、3 = FR_NOT_READY、
       11 = FR_INVALID_DRIVE、12 = FR_NOT_ENABLED、13 = FR_NO_FILESYSTEM 等），
       这样 tools/status.ps1 一眼就能看出是"卡通了但文件系统不认"还是"驱动/接线问题"。 */
    return g_mounted ? 0 : (int)fr;
}

/* 读取并解析一个音色文件：先按 DX7 .syx（FM）尝试，再按文本音色解析 */
int synth_port_patch_load_file(const char *path) {
    if (!g_mounted) return -1;
    if (f_open(&g_file, path, FA_READ) != FR_OK) return -2;

    UINT got = 0;
    const FRESULT fr = f_read(&g_file, g_text, sizeof(g_text) - 1u, &got);
    f_close(&g_file);
    if (fr != FR_OK) return -3;

    /* 1) DX7 .syx：32 音色 bank 或单音色 dump（第 0 个音色） */
    const int n = synth_load_fm_sysex((const uint8_t *)g_text, (uint32_t)got, 0);
    if (n > 0) return 0;

    /* 2) 文本音色 */
    g_text[got < sizeof(g_text) ? got : sizeof(g_text) - 1u] = '\0';
    patch_set_defaults(&g_loaded_patch);
    if (patch_from_text(g_text, &g_loaded_patch) < 0) return -4;
    synth_set_patch_ptr(&g_loaded_patch);        /* 引擎会在下一个渲染块拷贝走 */
    return 0;
}

/* 每通道一份音色缓冲：不能 16 个通道共用一个 —— synth_set_channel_patch_data() 只是把
   指针挂给引擎，音频线程下一块才拷贝，同一轮里挂多个指针就会全部指向最后那份内容。 */
static synth_patch_t g_chan_patch_buf[16];
static int16_t       g_chan_seen[16];    /* 上一次看到的通道音色号（-1 = 还没处理过） */
static uint8_t       g_chan_force[16];   /* 1 = 本次必须重新从卡上取（开机恢复映射之后） */

/* 从卡上给某个通道取 /synth/NNN.syn 覆盖该通道的音色内容（没有该文件返回负值） */
static int LoadChannelPatchFromCard(uint8_t c, uint16_t idx) {
    char path[64];
    snprintf(path, sizeof(path), SYNTH_PORT_PATCH_DIR "/%03u.syn", (unsigned)idx);
    if (f_open(&g_file, path, FA_READ) != FR_OK) return -1;

    UINT got = 0u;
    const FRESULT fr = f_read(&g_file, g_text, sizeof(g_text) - 1u, &got);
    f_close(&g_file);
    if (fr != FR_OK || got == 0u) return -2;

    g_text[got < sizeof(g_text) ? got : sizeof(g_text) - 1u] = '\0';
    patch_set_defaults(&g_chan_patch_buf[c]);
    if (patch_from_text(g_text, &g_chan_patch_buf[c]) <= 0) return -3;   /* 返回字段个数 */
    return synth_set_channel_patch_data(c, &g_chan_patch_buf[c]);
}

/* 主循环轮询：① 主音色号变了 → 先看 QSPI 采样库，再看卡上 /synth/NNN.ins|.syn
                 ② 任一通道音色号变了 → 从卡上取该通道的 .syn（没有就保持内置音色） */
void synth_port_patch_service(void) {
    const int16_t cur = (int16_t)synth_get_patch_index();
    if (!g_mounted) { g_last_request = cur; return; }

    if (cur != g_last_request) {
        g_last_request = cur;
        if (cur >= 0) {
            if (synth_port_lib_has_pc((uint8_t)cur)) {
                /* QSPI 库优先：零拷贝、开机就在，比卡快得多。想用卡上的版本就把库里
                   那个 PC 换成别的号（pack 时决定）。 */
                (void)synth_port_lib_load_pc((uint8_t)cur, NULL);
            } else {
                char path[64];
                snprintf(path, sizeof(path), SYNTH_PORT_PATCH_DIR "/%03d.ins", (int)cur);
                if (synth_port_instrument_load(path) <= 0) {
                    /* 没有采样乐器就试文本音色（.syn / DX7 .syx） */
                    snprintf(path, sizeof(path), SYNTH_PORT_PATCH_DIR "/%03d.syn", (int)cur);
                    (void)synth_port_patch_load_file(path);
                }
            }
        }
    }

    for (uint8_t c = 0u; c < 16u; ++c) {
        const uint16_t idx = synth_get_channel_patch(c);
        const int16_t cur_ch = (idx == 0xFFFFu) ? (int16_t)-1 : (int16_t)idx;
        const uint8_t force = g_chan_force[c];
        if (cur_ch == g_chan_seen[c] && !force) continue;
        g_chan_seen[c] = cur_ch;
        g_chan_force[c] = 0u;
        if (cur_ch >= 0) (void)LoadChannelPatchFromCard(c, (uint16_t)cur_ch);
    }
}

int synth_port_patch_save_file(uint16_t index, const synth_patch_t *p) {
    if (!g_mounted || p == NULL) return -1;
    char path[64];
    snprintf(path, sizeof(path), SYNTH_PORT_PATCH_DIR "/%03d.syn", (int)index);
    const int len = patch_to_text(p, g_text, sizeof(g_text));
    if (len < 0) return -2;
    if (f_open(&g_file, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return -3;
    UINT wrote = 0;
    const FRESULT fr = f_write(&g_file, g_text, (UINT)len, &wrote);
    f_close(&g_file);
    return (fr == FR_OK && wrote == (UINT)len) ? 0 : -4;
}

/* ---------------------------------------------------------------------------
   SD 读写自检 + 内置音色导出（挂载成功后调用一次，见 Core/Src/main.c 第 9 步）

   目的：一张卡插上去以后，不接 PC 也能知道"目录 / 写通路 / 读通路 / 文本解析"到底行不行；
        顺便把 15 个内置音色导出成 /synth/NNN.syn 模板 —— **只写不存在的文件**，
        所以用户在卡上改过的音色不会被开机覆盖掉。
   返回位图（存进 g_app_status.sd_selftest，用 tools/status.ps1 读）：
     bit0 目录可用（/synth 已存在或新建成功）
     bit1 至少写成功一个文件
     bit2 读回成功（读到的字节数 > 0）
     bit3 读回的文本能解析成音色（patch_from_text 返回值 > 0 = 解析成功的字段数）
     bit4 导出时跳过过已存在的文件（= 卡上本来就有东西，没被覆盖）
     bit5 走真实加载路径（synth_port_patch_load_file("/synth/000.syn")）成功
   synth_port_sd_exported() 给出本次真正新写出的文件数（新卡第一次上电应为 15，之后为 0）。
   ⇒ 全部通过应为 0x2F；只要卡上已有文件，bit4 也会亮，所以稳态是 0x3F（bit1 只在本次真写了东西时亮）。
--------------------------------------------------------------------------- */
static uint32_t g_sd_selftest = 0u;
static uint32_t g_sd_exported = 0u;

uint32_t synth_port_sd_selftest(void) {
    g_sd_selftest = 0u;
    g_sd_exported = 0u;
    if (!g_mounted) return g_sd_selftest;

    /* 1) 目录 */
    const FRESULT fm = f_mkdir(SYNTH_PORT_PATCH_DIR);
    if (fm == FR_OK || fm == FR_EXIST) {
        g_sd_selftest |= 0x01u;
    } else {
        return g_sd_selftest;              /* 目录都建不了，后面没必要试 */
    }

    /* 2) 导出内置音色模板（已存在的不覆盖） */
    for (uint32_t i = 0u; i < (uint32_t)synth_factory_bank_size; ++i) {
        char path[64];
        snprintf(path, sizeof(path), SYNTH_PORT_PATCH_DIR "/%03u.syn", (unsigned)i);

        FIL probe;
        if (f_open(&probe, path, FA_READ) == FR_OK) {
            f_close(&probe);
            g_sd_selftest |= 0x10u;        /* 已有这个文件 → 记录"没覆盖" */
            continue;
        }
        if (synth_port_patch_save_file((uint16_t)i, &synth_factory_bank[i]) == 0) {
            ++g_sd_exported;
            g_sd_selftest |= 0x02u;
        }
    }

    /* 3) 读回自检：000.syn 读出来、解析一遍 */
    if (f_open(&g_file, SYNTH_PORT_PATCH_DIR "/000.syn", FA_READ) != FR_OK) return g_sd_selftest;
    UINT got = 0u;
    const FRESULT fr = f_read(&g_file, g_text, sizeof(g_text) - 1u, &got);
    f_close(&g_file);
    if (fr != FR_OK || got == 0u) return g_sd_selftest;
    g_sd_selftest |= 0x04u;

    g_text[got < sizeof(g_text) ? got : sizeof(g_text) - 1u] = '\0';
    synth_patch_t tmp;
    patch_set_defaults(&tmp);
    /* 注意 patch_from_text() 成功时返回的是"解析成功的字段个数"（>0），不是 0（0 只可能是空文件）。 */
    const int parsed = patch_from_text(g_text, &tmp);
    if (parsed > 0) g_sd_selftest |= 0x08u;

    /* 4) 走一遍真正的加载路径（发 PC 切音色时引擎走的就是这个函数） */
    if (synth_port_patch_load_file(SYNTH_PORT_PATCH_DIR "/000.syn") == 0) g_sd_selftest |= 0x20u;
    return g_sd_selftest;
}

uint32_t synth_port_sd_exported(void) { return g_sd_exported; }

/* ---- 写回 / 覆盖自检：证明"卡上的内容才是权威" ---------------------------------------
   前面的自检只能证明"卡上的文件读得出来、解析得动"，但证明不了**覆盖生效**
   （卡上内容与内置表相同的话，解析成功也不能区分数据来自哪边）。
   做法：取内置音色 0（Init Saw，cutoff = 150）的副本、只把 cutoff 改成 40，
         覆盖写到 /synth/000.syn → 读回解析，看 cutoff 是不是 40（是 ⇒ 数据来自卡）
         → 再把内置原件写回去（恢复卡上模板）→ 再读回确认 cutoff 回到内置值。
   用 000 号：它就是开机默认音色，测试完会恢复成原件，不会在卡上留下奇怪的音色。
   返回位图：bit0 覆盖写成功 / bit1 读回解析出的 cutoff = 测试值 / bit2 恢复原件成功 /
             bit3 恢复后读回解析出的 cutoff = 内置值。 */
#define SD_WTEST_CUTOFF 40u

static int SdReadPatchCutoff(uint16_t *out_cutoff) {
    UINT got = 0u;
    synth_patch_t tmp;

    if (f_open(&g_file, SYNTH_PORT_PATCH_DIR "/000.syn", FA_READ) != FR_OK) return -1;
    const FRESULT fr = f_read(&g_file, g_text, sizeof(g_text) - 1u, &got);
    f_close(&g_file);
    if (fr != FR_OK || got == 0u) return -2;

    g_text[got < sizeof(g_text) ? got : sizeof(g_text) - 1u] = '\0';
    patch_set_defaults(&tmp);
    if (patch_from_text(g_text, &tmp) <= 0) return -3;   /* 返回字段个数，不是 0 */
    if (out_cutoff != 0) *out_cutoff = (uint16_t)tmp.cutoff;
    return 0;
}

uint32_t synth_port_sd_write_test(void) {
    uint32_t bits = 0u;
    uint16_t seen = 0u;

    if (!g_mounted) return bits;

    /* 1) 覆盖写：内置音色 0 的副本，只改 cutoff */
    synth_patch_t mod = synth_factory_bank[0];
    mod.cutoff = (uint8_t)SD_WTEST_CUTOFF;
    if (synth_port_patch_save_file(0u, &mod) == 0) bits |= 0x01u;

    /* 2) 读回解析：cutoff 应该是测试值（说明卡上内容真的生效了） */
    if (SdReadPatchCutoff(&seen) == 0 && seen == (uint16_t)SD_WTEST_CUTOFF) bits |= 0x02u;

    /* 3) 把内置原件写回去，恢复卡上模板 */
    if (synth_port_patch_save_file(0u, &synth_factory_bank[0]) == 0) bits |= 0x04u;

    /* 4) 再读回确认已恢复 */
    if (SdReadPatchCutoff(&seen) == 0 && seen == (uint16_t)synth_factory_bank[0].cutoff) bits |= 0x08u;

    return bits;
}

/* ---- 开机状态持久化（/synth/state.cfg）------------------------------------------
   存的是"16 个通道分别用哪个音色号"（就是 Program Change 号），不存音色内容本身 ——
   音色内容在 /synth/NNN.syn 里。开机流程：恢复索引 → patch_service 发现索引变化
   → 从卡加载对应文件，与手动切音色完全同一条路径，所以生效语义一致。
   文本格式（一行一个通道，方便人肉改）：
       synth-state 1
       ch0=5
       ch15=0
   写盘时机：主循环里轮询（synth_port_state_service），索引变化后静默 2 s 才落盘，
   避免连续切音色时反复擦写。两个结果给 status.ps1 看：state_load / state_saves。 */
#define SYNTH_STATE_FILE      SYNTH_PORT_PATCH_DIR "/state.cfg"
#define SYNTH_STATE_QUIET_MS  2000u

static int16_t  g_state_seen[16];       /* 上一次看到的通道音色号（-1 = 尚未建立基线） */
static uint32_t g_state_dirty_ms = 0u;  /* 0 = 没有待写；否则 = 最近一次变化的时间戳 */
static int      g_state_load = -1;      /* -1 = 没文件/读失败；>= 0 = 恢复的通道数 */
static uint32_t g_state_saves = 0u;

static void StateSnapshotChannels(void) {
    for (int c = 0; c < 16; ++c) {
        const uint16_t idx = synth_get_channel_patch((uint8_t)c);
        g_state_seen[c] = (idx == 0xFFFFu) ? (int16_t)-1 : (int16_t)idx;
    }
}

/* 开机时把 16 个通道在卡上的 .syn 内容各读一次。调用点在 main() 第 8 步（起音频之前），
   所以读卡不会饿着预渲染环；同时把 g_chan_seen 设成当前音色号，主循环里的 patch_service
   就不会再为这些通道重复读一遍卡。读不到文件就保持内置音色。 */
static void LoadAllChannelPatchesFromCard(void) {
    for (uint8_t c = 0u; c < 16u; ++c) {
        const uint16_t idx = synth_get_channel_patch(c);
        const int16_t v = (idx == 0xFFFFu) ? (int16_t)-1 : (int16_t)idx;
        g_chan_force[c] = 0u;
        g_chan_seen[c] = v;
        if (v >= 0) (void)LoadChannelPatchFromCard(c, (uint16_t)v);
    }
}

/* 读 state.cfg 并把 16 个通道音色号排进引擎队列；返回恢复的通道数（-1 = 没有文件） */
int synth_port_state_load(void) {
    if (!g_mounted) return -1;
    if (f_open(&g_file, SYNTH_STATE_FILE, FA_READ) != FR_OK) {
        StateSnapshotChannels(); LoadAllChannelPatchesFromCard(); return -1;
    }

    UINT got = 0u;
    const FRESULT fr = f_read(&g_file, g_text, sizeof(g_text) - 1u, &got);
    f_close(&g_file);
    if (fr != FR_OK || got == 0u) {
        StateSnapshotChannels(); LoadAllChannelPatchesFromCard(); return -1;
    }
    g_text[got < sizeof(g_text) ? got : sizeof(g_text) - 1u] = '\0';

    /* 只认 "chNN=<数字>"，其它行一律跳过（以后加字段不会破坏老文件） */
    int restored = 0;
    const char *p = g_text;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (p[0] == 'c' && p[1] == 'h') {
            const char *q = p + 2;
            int ch = 0, val = 0;
            while (*q >= '0' && *q <= '9') { ch = ch * 10 + (*q - '0'); ++q; }
            if (*q == '=') {
                ++q;
                while (*q >= '0' && *q <= '9') { val = val * 10 + (*q - '0'); ++q; }
                if (ch >= 0 && ch < 16 && val >= 0 && val < (int)synth_factory_bank_size) {
                    synth_set_channel_patch((uint8_t)ch, (uint16_t)val);
                    ++restored;
                }
            }
            p = q;
        }
        while (*p != '\0' && *p != '\n') ++p;
    }
    g_state_load = restored;
    StateSnapshotChannels();
    /* 卡上内容一次性读好（g_chan_seen 同步更新）⇒ 主循环不会再重复读卡；没写进 state.cfg
       的通道也在这里读它的 .syn，保持"卡上文件优先于内置音色"的语义。 */
    LoadAllChannelPatchesFromCard();
    return restored;
}

/* 立刻把当前 16 个通道音色号写进 state.cfg */
int synth_port_state_save(void) {
    if (!g_mounted) return -1;
    int n = snprintf(g_text, sizeof(g_text), "synth-state 1\n");
    if (n < 0) return -2;
    for (int c = 0; c < 16 && n > 0 && (size_t)n < sizeof(g_text); ++c) {
        const uint16_t idx = synth_get_channel_patch((uint8_t)c);
        const int m = snprintf(g_text + n, sizeof(g_text) - (size_t)n, "ch%d=%u\n", c,
                               (unsigned)((idx == 0xFFFFu) ? 0u : idx));
        if (m < 0) break;
        n += m;
    }
    if (f_open(&g_file, SYNTH_STATE_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return -3;
    UINT wrote = 0u;
    const FRESULT fr = f_write(&g_file, g_text, (UINT)n, &wrote);
    f_close(&g_file);
    if (fr != FR_OK || wrote != (UINT)n) return -4;
    ++g_state_saves;
    return 0;
}

/* 主循环轮询：索引变了就记脏，静默 2 s 后落盘 */
void synth_port_state_service(void) {
    if (!g_mounted) return;
    if (g_state_seen[0] < 0 && g_state_seen[15] < 0) { StateSnapshotChannels(); return; }

    int changed = 0;
    for (int c = 0; c < 16; ++c) {
        const uint16_t idx = synth_get_channel_patch((uint8_t)c);
        const int16_t cur = (idx == 0xFFFFu) ? (int16_t)-1 : (int16_t)idx;
        if (cur != g_state_seen[c]) { g_state_seen[c] = cur; changed = 1; }
    }
    const uint32_t now = HAL_GetTick();
    if (changed) { g_state_dirty_ms = (now != 0u) ? now : 1u; return; }
    if (g_state_dirty_ms != 0u && (uint32_t)(now - g_state_dirty_ms) >= SYNTH_STATE_QUIET_MS) {
        if (synth_port_state_save() == 0) g_state_dirty_ms = 0u;
        else g_state_dirty_ms = now;        /* 写失败：下个静默窗口再试 */
    }
}

int      synth_port_state_load_result(void) { return g_state_load; }
uint32_t synth_port_state_save_count(void)  { return g_state_saves; }

int synth_port_sd_is_mounted(void) { return g_mounted ? 1 : 0; }

/* ── RAM overlay 模块：把卡上的 /synth/ui.bin 写进 QSPI（0x7F0000 起）────────────
   文件布局 = synth_module_hdr_t（36 B）+ 裸镜像。

   顺序讲究（2026-10-20 踩过两个坑）：
     1) 头部必须**最后**写：中途失败/掉电时头部无效 → 下次开机自动重导；
     2) 镜像从 offset+36 开始，**不与 256 B 页边界对齐** ⇒ 必须按页边界切块，
        否则 page_program() 会拒收（曾经镜像写不进去、头部却写成功 → 开机 CRC -3）；
     3) 头部一致不等于镜像完整：还要对映射地址算一次 CRC 才算"已是最新"。
   返回：0 = 已是最新 / 1 = 已导入 / -1 = 没挂载或没文件 / -2 = 头非法 / -3 = 擦写或校验失败 */
#define SYNTH_MODULE_FILE "0:/synth/ui.bin"

/* 板上镜像的数据 CRC：直接对映射地址算，与 module.c 校验的是同一块内存 */
static uint32_t module_image_crc(uint32_t image_bytes)
{
    if (synth_port_qspi_ensure_mmap() != 0) return 0u;
    const void *p = (const void *)(uintptr_t)(SYNTH_QSPI_MAP_BASE + SYNTH_QSPI_MODULE_OFFSET +
                                             (uint32_t)sizeof(synth_module_hdr_t));
    return synth_module_crc32(p, image_bytes);
}

int synth_port_module_import_from_card(void)
{
    if (!g_mounted) return -1;

    FIL f;
    if (f_open(&f, SYNTH_MODULE_FILE, FA_READ) != FR_OK) return -1;

    synth_module_hdr_t h;
    UINT got = 0;
    if (f_read(&f, &h, sizeof(h), &got) != FR_OK || got != sizeof(h)) {
        f_close(&f);
        return -2;
    }
    if (h.magic != SYNTH_MODULE_MAGIC || h.version != SYNTH_MODULE_VERSION ||
        h.abi != SYNTH_MODULE_ABI || h.hdr_bytes != (uint16_t)sizeof(synth_module_hdr_t) ||
        h.image_bytes == 0u || (h.image_bytes & 3u) != 0u ||
        (uint32_t)h.hdr_bytes + h.image_bytes > SYNTH_QSPI_MODULE_MAX) {
        f_close(&f);
        return -2;
    }

    /* 板上已经是最新？头部一致 **且** 镜像 CRC 对得上，才跳过 */
    synth_module_hdr_t cur = {0};
    if (synth_port_qspi_read(SYNTH_QSPI_MODULE_OFFSET, &cur, (uint32_t)sizeof(cur)) == 0 &&
        cur.magic == SYNTH_MODULE_MAGIC && cur.build_id == h.build_id &&
        cur.crc32 == h.crc32 && cur.image_bytes == h.image_bytes &&
        module_image_crc(cur.image_bytes) == h.crc32) {
        f_close(&f);
        return 0;
    }
    /* 板上这份是用 USB 上传的（头部打了 USB 标记）→ 卡上那份是旧的，不要覆盖：
       原厂/首次安装走卡，之后的迭代走 tools\\upload_module.py。
       ⚠ 标记位置：upload_module.py 打的是头部的 `reserved[1]`（偏移 32），早期代码
       查的是 `reserved[0]`（偏移 28）→ 结果每次上电都被卡上的旧 ui.bin 顶回去。
       两个都认，避免以后再错位。 */
    if (cur.magic == SYNTH_MODULE_MAGIC &&
        (cur.reserved[0] == SYNTH_MODULE_USB_FLAG || cur.reserved[1] == SYNTH_MODULE_USB_FLAG)) {
        f_close(&f);
        return 0;
    }

    const uint32_t total   = (uint32_t)h.hdr_bytes + h.image_bytes;
    const uint32_t sectors = (total + SYNTH_QSPI_SECTOR_BYTES - 1u) / SYNTH_QSPI_SECTOR_BYTES;
    for (uint32_t i = 0; i < sectors; ++i) {
        if (synth_port_qspi_erase_sector(SYNTH_QSPI_MODULE_OFFSET + i * SYNTH_QSPI_SECTOR_BYTES) < 0) {
            f_close(&f);
            (void)synth_port_qspi_mmap_reenter();
            return -3;
        }
    }

    /* 镜像：按页边界切块（起始地址 = offset + 36，不对齐页） */
    static uint8_t s_page[SYNTH_QSPI_PAGE_BYTES];
    uint32_t off  = SYNTH_QSPI_MODULE_OFFSET + (uint32_t)sizeof(h);
    uint32_t left = h.image_bytes;
    while (left > 0u) {
        uint32_t chunk = SYNTH_QSPI_PAGE_BYTES - (off % SYNTH_QSPI_PAGE_BYTES);
        if (chunk > left) chunk = left;
        if (f_read(&f, s_page, (UINT)chunk, &got) != FR_OK || got != chunk ||
            synth_port_qspi_write_enable() < 0 || synth_port_qspi_page_program(off, s_page, chunk) < 0) {
            f_close(&f);
            (void)synth_port_qspi_mmap_reenter();
            return -3;
        }
        off  += chunk;
        left -= chunk;
    }

    /* 头部最后写 */
    if (synth_port_qspi_write_enable() < 0 ||
        synth_port_qspi_page_program(SYNTH_QSPI_MODULE_OFFSET, &h, (uint32_t)sizeof(h)) < 0) {
        f_close(&f);
        (void)synth_port_qspi_mmap_reenter();
        return -3;
    }
    f_close(&f);

    synth_port_qspi_cache_invalidate();
    (void)synth_port_qspi_mmap_reenter();      /* 回到映射模式（采样库/模块都靠它） */

    /* 回读校验：头部 + 镜像 CRC 都对才算导入成功 */
    if (synth_port_qspi_read(SYNTH_QSPI_MODULE_OFFSET, &cur, (uint32_t)sizeof(cur)) != 0 ||
        cur.magic != SYNTH_MODULE_MAGIC || cur.build_id != h.build_id ||
        cur.crc32 != h.crc32 || module_image_crc(cur.image_bytes) != h.crc32) {
        return -3;
    }
    return 1;
}

#else /* !SYNTH_PORT_SD_ENABLE —— 未启用 FatFs：全部退化为“无卡”行为 */

int synth_port_sd_mount(void) { return -1; }
int synth_port_sd_is_mounted(void) { return 0; }
int synth_port_module_import_from_card(void) { return 0; }
void synth_port_patch_service(void) { }
int synth_port_patch_load_file(const char *path) { (void)path; return -1; }
int synth_port_patch_save_file(uint16_t index, const synth_patch_t *p) {
    (void)index; (void)p; return -1;
}
uint32_t synth_port_sd_selftest(void) { return 0u; }
uint32_t synth_port_sd_exported(void) { return 0u; }
uint32_t synth_port_sd_write_test(void) { return 0u; }
int      synth_port_state_load(void) { return -1; }
int      synth_port_state_save(void) { return -1; }
void     synth_port_state_service(void) { }
int      synth_port_state_load_result(void) { return -1; }
uint32_t synth_port_state_save_count(void) { return 0u; }

#endif /* SYNTH_PORT_SD_ENABLE */
