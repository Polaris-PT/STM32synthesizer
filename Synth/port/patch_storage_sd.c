/* SD 卡音色存储：/synth/NNN.syn（0:/synth）。2
   仅在主循环调用（FATFS 不可重入、不可在音频中断里使用）。 */
#include "synth_port_config.h"
#include "synth/synth.h"
#include "../src/app/patch_text.h"

#include "ff.h"
#include <string.h>
#include <stdio.h>

static FATFS       g_fs;
static int         g_mounted = 0;
static FIL         g_file;
static char        g_text[SYNTH_PORT_PATCH_MAXLEN];
static synth_patch_t g_loaded_patch;
static int16_t     g_last_request = -1;

int synth_port_sd_mount(void) {
    const FRESULT fr = f_mount(&g_fs, "0:", 1);
    g_mounted = (fr == FR_OK) ? 1 : 0;
    return g_mounted ? 0 : -1;
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

/* 主循环轮询：Program Change 请求的音色若在 SD 上有对应文件则加载 */
void synth_port_patch_service(void) {
    const int16_t cur = (int16_t)synth_get_patch_index();
    if (cur == g_last_request) return;
    g_last_request = cur;
    if (!g_mounted || cur < 0) return;

    char path[64];
    /* 1) 采样乐器清单 */
    snprintf(path, sizeof(path), SYNTH_PORT_PATCH_DIR "/%03d.ins", (int)cur);
    if (synth_port_instrument_load(path) > 0) return;
    /* 2) 其它音色文件（.syx / .syn）——没有就继续用内置音色 */
    snprintf(path, sizeof(path), SYNTH_PORT_PATCH_DIR "/%03d.syn", (int)cur);
    (void)synth_port_patch_load_file(path);

    /* 也支持按名字：/synth/<音色名>.syn */
    synth_stats_t st;
    synth_get_stats(&st);
    (void)st;
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
