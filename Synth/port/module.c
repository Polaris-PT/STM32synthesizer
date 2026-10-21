/* RAM overlay 模块加载器（宿主侧）。设计说明见 module.h / module_api.h。
 *
 * 一句话：QSPI 顶部 64 KB 里放"头 + 裸镜像"，上电校验通过后整体拷到 AXI SRAM 顶部
 * （0x24070000，链接脚本里的 MODULE_RAM 段），再调用镜像里的入口函数拿到回调表。
 * 之后宿主每圈调 synth_module_tick()，按键事件调 synth_module_key()。
 * 模块不能直接引用宿主符号，只能通过 module_api.h 里的函数指针表。
 */
#include "module.h"

#include <string.h>

#include "stm32h7xx_hal.h"
#include "qspi_flash.h"
#include "synth_port.h"
#include "synth_port_config.h"
#include "debug_uart.h"
#include "synth/synth.h"

/* 模块回报的探针值（宿主每圈拷进 g_app_status.module_probe[]） */
volatile uint32_t g_module_probe[2] = {0u, 0u};

static const synth_module_cbs_t *s_cbs;      /* 非空 = 已加载 */
static int      s_state = -1;
static uint32_t s_crc;
static uint32_t s_build_id;
static uint32_t s_ticks;
static uint32_t s_last_tick;

static void module_probe(uint16_t idx, uint32_t value);

/* ── CRC32（多项式 0xEDB88320，初值/结果异或 0xFFFFFFFF）—— 与 tools/make_module.py 一致 ── */
static uint32_t crc32_buf(const uint8_t *p, uint32_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    while (n-- > 0u) {
        crc ^= (uint32_t)(*p++);
        for (unsigned k = 0; k < 8u; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

uint32_t synth_module_crc32(const void *data, uint32_t len) {
    return crc32_buf((const uint8_t *)data, len);
}

static const volatile synth_module_hdr_t *qspi_hdr(void) {
    return (const volatile synth_module_hdr_t *)(uintptr_t)(SYNTH_QSPI_MAP_BASE + SYNTH_QSPI_MODULE_OFFSET);
}

/* ── 宿主提供给模块的 API 实现 ── */
static void api_get_status(synth_module_status_t *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    synth_stats_t st;
    synth_get_stats(&st);
    out->cpu_permille = synth_cpu_load_permille();
    out->uptime_ms    = HAL_GetTick();
    out->midi_bytes   = st.midi_bytes;
    out->midi_dropped = st.midi_dropped;
    out->notes_on     = st.notes_on;
    out->steals       = st.steals;
    out->xruns        = g_synth_audio_xruns;
    out->underruns    = g_synth_audio_underruns;
    out->lib_count    = synth_port_lib_count();
    out->lib_serial   = synth_port_lib_serial();
    out->sample_rate  = (uint16_t)SYNTH_SR_DEFAULT;
    out->patch_index  = synth_get_patch_index();
    for (unsigned i = 0; i < 16u; ++i) {
        out->chan_patch[i] = synth_get_channel_patch((uint8_t)i);
    }
    out->active_voices = st.active_voices;
    out->peak_voices   = st.peak_voices;
}

static void api_log(const char *msg) {
#if SYNTH_PORT_DEBUG_UART
    dbg_log("mod", "%s", msg ? msg : "");
#else
    (void)msg;
#endif
}

/* 库查询的适配层：端口层的老接口是 (index, name16, uint8_t*, uint8_t*)，
   这里转成模块侧的 (int index, char*, uint16_t name_len, uint16_t*, uint16_t*)。 */
static int api_lib_info(int index, char *name, uint16_t name_len, uint16_t *pc, uint16_t *zones) {
    if (index < 0) return -1;
    char    tmp[16];
    uint8_t p = 0xFFu;
    uint8_t z = 0u;
    const int rc = synth_port_lib_info((uint32_t)index, tmp, &p, &z);
    if (rc != 0) return rc;
    if (name != NULL && name_len > 0u) {
        unsigned n = 0;
        while ((n + 1u) < (unsigned)name_len && tmp[n] != '\0') { name[n] = tmp[n]; ++n; }
        name[n] = '\0';
    }
    if (pc != NULL)    *pc    = p;
    if (zones != NULL) *zones = z;
    return 0;
}

static const synth_module_api_t g_api = {
    SYNTH_MODULE_ABI,
    HAL_GetTick,
    api_get_status,
    synth_set_patch,
    synth_set_channel_patch,
    api_lib_info,
    synth_port_sd_is_mounted,
    api_log,
    module_probe,
};

static void module_probe(uint16_t idx, uint32_t value) {
    g_module_probe[idx & 1u] = value;
}

/* 卸载（只清"已加载"标记，不动 RAM 里的代码）：USB 上传新镜像后要能重新加载。
   注意：调用点必须在主循环上下文（模块的 tick 只从主循环来），否则会踩到正在执行的代码。 */
void synth_module_unload(void) {
    s_cbs   = NULL;
    s_state = 0;
}

int synth_module_init(void) {
    if (s_cbs != NULL) return 0;                    /* 幂等：重复调用不重复拷 */

    /* 读头部靠的是内存映射指针 ⇒ 必须先确保控制器在映射模式。导入器的间接读会对
       abort 掉映射（skip 分支曾经就这样把映射留在了关闭状态，随后这里一读就 BusFault）。 */
    if (synth_port_qspi_ensure_mmap() != 0) {
        s_state = -7;
        return -7;
    }
    /* QSPI 映射区是**可缓存**的：如果 CPU 之前读过这块（开机已经读过一次头部/镜像），
       缓存里可能还是旧镜像 —— 不 invalidate 就会把旧代码拷进 RAM（症状：USB 上传后
       module_crc 仍是旧值、屏幕没变化）。 */
    synth_port_qspi_cache_invalidate();

    const volatile synth_module_hdr_t *h = qspi_hdr();
    if (h->magic != SYNTH_MODULE_MAGIC || h->version != SYNTH_MODULE_VERSION) {
        s_state = -1;
        return -1;
    }
    if (h->abi != SYNTH_MODULE_ABI || h->hdr_bytes != (uint16_t)sizeof(synth_module_hdr_t)) {
        s_state = -2;
        return -2;
    }
    const uint32_t n     = h->image_bytes;
    const uint32_t entry = h->entry_offset;
    if (n == 0u || (n & 3u) != 0u || n > SYNTH_MODULE_RAM_BYTES || n > SYNTH_QSPI_MODULE_MAX ||
        (entry & 3u) != 0u || entry + 4u > n) {
        s_state = -4;
        return -4;
    }

    const uint8_t *src = (const uint8_t *)(uintptr_t)(SYNTH_QSPI_MAP_BASE + SYNTH_QSPI_MODULE_OFFSET +
                                                      sizeof(synth_module_hdr_t));
    const uint32_t crc = crc32_buf(src, n);
    if (crc != h->crc32) {
        s_state = -3;
        return -3;
    }

    /* 拷到 RAM：源是 QSPI 映射区（可缓存，导入后已 invalidate），目标是 AXI SRAM */
    memcpy((void *)(uintptr_t)SYNTH_MODULE_RAM_BASE, src, n);

    /* 关键：AXI SRAM 走 D-Cache 写回，刚 memcpy 进去的代码可能只躺在 D-Cache 里；直接跳过去
       执行时，取指并不查 D-Cache，读到的是内存里的旧内容 → 执行垃圾指令（症状：整机卡死、
       连 HardFault 都记不上）。所以先把数据缓存 clean 到内存，再让 I-Cache 整体作废。 */
    SCB_CleanDCache_by_Addr((uint32_t *)(uintptr_t)SYNTH_MODULE_RAM_BASE, (int32_t)((n + 31u) & ~31u));
    SCB_InvalidateICache();
    __DSB();
    __ISB();

    /* 入口地址必须带上 Thumb 位（bit0=1）：nm 报出来的符号值是偶数地址（.text 里的对齐
       地址），直接 bx 过去等于 T=0 → UsageFault/INVSTATE（CFSR bit17），表现为一进模块就崩。 */
    const synth_module_entry_fn fn =
        (synth_module_entry_fn)(uintptr_t)(SYNTH_MODULE_RAM_BASE + entry + 1u);
    const synth_module_cbs_t *cbs = fn(&g_api);
    if (cbs == NULL || cbs->abi != SYNTH_MODULE_ABI || cbs->tick == NULL) {
        s_state = -5;
        return -5;
    }

    s_cbs      = cbs;
    s_crc      = crc;
    s_build_id = h->build_id;
    s_state    = 0;
    s_last_tick = HAL_GetTick();
#if SYNTH_PORT_DEBUG_UART
    dbg_log("mod", "loaded: %lu B, crc=0x%lx, build=0x%lx", (unsigned long)n,
            (unsigned long)s_crc, (unsigned long)s_build_id);
#endif
    return 0;
}

void synth_module_tick(void) {
    if (s_cbs == NULL) return;
    const uint32_t now = HAL_GetTick();
    if ((uint32_t)(now - s_last_tick) < 10u) return;   /* 100 Hz 上限：UI 不需要更快 */
    s_last_tick = now;
    ++s_ticks;
    s_cbs->tick(now);
}

void synth_module_key(uint8_t pressed) {
    if (s_cbs != NULL && s_cbs->key != NULL) s_cbs->key(pressed ? 1u : 0u);
}

int      synth_module_loaded(void)     { return (s_cbs != NULL) ? 1 : 0; }
int      synth_module_state(void)      { return (s_cbs != NULL) ? 0 : s_state; }
uint32_t synth_module_crc(void)        { return s_crc; }
uint32_t synth_module_build_id(void)   { return s_build_id; }
uint32_t synth_module_ticks(void)      { return s_ticks; }
