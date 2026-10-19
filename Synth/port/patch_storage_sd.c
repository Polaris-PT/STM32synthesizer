/* SD 卡音色存储：/synth/NNN.syn（0:/synth）。
   仅在主循环调用（FATFS 不可重入、不可在音频中断里使用）。
   CubeMX 里还没启用 FatFs 时（SYNTH_PORT_SD_ENABLE=0）退化为空实现，保证工程可编译。 */
#include "synth_port_config.h"
#include "synth_port.h"      /* synth_port_instrument_load() 的声明（实现在 sample_storage_sd.c） */
#include "synth/synth.h"

#if SYNTH_PORT_SD_ENABLE

#include "../src/app/patch_text.h"
#include "ff.h"
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

#else /* !SYNTH_PORT_SD_ENABLE —— 未启用 FatFs：全部退化为“无卡”行为 */

int synth_port_sd_mount(void) { return -1; }
void synth_port_patch_service(void) { }
int synth_port_patch_load_file(const char *path) { (void)path; return -1; }
int synth_port_patch_save_file(uint16_t index, const synth_patch_t *p) {
    (void)index; (void)p; return -1;
}

#endif /* SYNTH_PORT_SD_ENABLE */
