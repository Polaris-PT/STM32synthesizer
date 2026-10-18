/* 阶段 3：QSPI flash 采样（内存映射只读访问）。
 *
 * 前置条件（CubeMX 侧，见 docs/cubemx-checklist.md）：
 *   1) QUADSPI Bank1 四线：PB2=CLK、PB6=NCS、PD11=IO0、PD12=IO1、PD13=IO3、PE2=IO2
 *      （与 SAI3 Block B 音频引脚不冲突）
 *   2) 让 QUADSPI 进入内存映射模式后，flash 内容出现在 0x90000000
 *   3) **放开 MPU**：CubeMX 默认把 0x60000000–0xDFFFFFFF 设为 NO_ACCESS，
 *      必须为 0x90000000 增加一条可读区域，否则访问会 HardFault
 *
 * 布局约定：QSPI 里存原始 16bit 单声道 PCM（无文件头），
 * 采样数组地址 = 0x90000000 + 字节偏移，直接作为 zone->data 使用（零拷贝）。
 */
#include "synth_port_config.h"
#include "synth_port.h"
#include "synth/synth.h"

#ifndef SYNTH_QSPI_MAP_BASE
#define SYNTH_QSPI_MAP_BASE 0x90000000u
#endif

static synth_sample_instrument_t g_qspi_instrument;

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
