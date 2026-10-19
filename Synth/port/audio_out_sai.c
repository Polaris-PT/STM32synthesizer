/* SAI3 Block B + DMA 循环输出。
 *
 * 架构（2026-10-19 改）：**渲染在主循环，中断里只搬数据**。
 *
 * 之前是"半传输/传输完成中断里直接渲染整块"：复音一多，渲染耗时超过半缓冲时间
 * （128 帧 @48 kHz = 2.67 ms）就把优先级 0 的音频中断撑爆 —— 表现为
 *   ① DMA 欠载 → 声音断续/滋滋；
 *   ② USB 中断（优先级 5）被推迟 → MIDI 包丢失（note-off 丢了 = 音停不下来）；
 *   ③ 极端情况主循环完全跑不动 → 整块板像死机。
 *
 * 现在：主循环调 synth_port_audio_pump() 提前渲染 4 块到环形缓冲里；音频中断只在
 * 半缓冲结束时 memcpy 一块（~1 KB）并做 D-Cache 写回，耗时几十微秒。
 * 主循环来不及填时中断会重复上一块（并累加 g_synth_audio_underruns），
 * 声音会短暂"卡"一下，但不会把 USB/主循环拖死。
 */
#include "synth_port_config.h"
#include "synth/synth.h"
#include "debug_uart.h"

#include <string.h>

#if defined(SYNTH_SAI_HANDLE)

/* 一个渲染块的 word 数（128 帧 × 2 声道） */
#define SYNTH_AUDIO_BLOCK_WORDS (SYNTH_PORT_BLOCK_FRAMES * SYNTH_PORT_WORDS_PER_FRAME)
#define SYNTH_AUDIO_BLOCK_BYTES ((uint32_t)sizeof(int32_t) * SYNTH_AUDIO_BLOCK_WORDS)

/* 预渲染环形缓冲的块数：3 块 = 8 ms 余量（主循环偶尔被 USB/SD 拖慢也不会欠载）。
   放 AXI SRAM（.ram_section）：DMA 访问不到 DTCM；这里的读写全由 CPU 完成，
   进 DMA 之前才做 D-Cache 写回。 */
#define SYNTH_AUDIO_RING_BLOCKS 3u

static int32_t SYNTH_PORT_AUDIO_RAM
    g_ring[SYNTH_AUDIO_RING_BLOCKS][SYNTH_AUDIO_BLOCK_WORDS] __attribute__((aligned(32)));
static int32_t SYNTH_PORT_AUDIO_RAM
    g_dma_buf[2u * SYNTH_AUDIO_BLOCK_WORDS] __attribute__((aligned(32)));

/* 单生产者（主循环）/ 单消费者（音频中断）环形缓冲。
   g_ring_w 只由主循环写，g_ring_r 只由中断写，所以不需要关中断。 */
static volatile uint32_t g_ring_w = 0u;
static volatile uint32_t g_ring_r = 0u;

/* 这几个计数器只被"看板子的人"（tools/status.ps1 用 arm-none-eabi-nm 按名字找符号）和串口日志
   读，C 代码里可能一个读者都没有。链接带 -fdata-sections + --gc-sections（LTO 下更激进），没人
   引用的段会被回收 → 符号消失后 status.ps1 报 "ELF 里找不到符号"。
   保持符号的办法是让 C 代码真的读它们：Core/Src/main.c 的 App_KeepObservablesAlive()。 */
volatile uint32_t g_synth_audio_xruns = 0u;      /* SAI 报错次数（HAL 回调） */
volatile uint32_t g_synth_audio_underruns = 0u;  /* 主循环没来得及填：中断重复了上一块 */
volatile uint32_t g_synth_audio_last_half = 0xFFFFFFFFu;
/* 存活探针：>0 = 正在音频 DMA 中断里。改架构后这里应该总是短暂为 1；
   长时间停在 1 说明卡在 memcpy/D-Cache 写回（几乎不可能）。 */
volatile uint32_t g_synth_audio_isr_depth = 0u;

static inline void audio_clean(void *buf, uint32_t bytes) {
    SCB_CleanDCache_by_Addr((uint32_t *)(uintptr_t)buf, (int32_t)bytes);
}

/* 中断里：从环里取一块填进 DMA 的那个半缓冲；环空就保持原内容（重复上一块）。 */
static inline void audio_serve_half(int32_t *dst) {
    const uint32_t r = g_ring_r;
    if (r != g_ring_w) {
        memcpy(dst, g_ring[r], SYNTH_AUDIO_BLOCK_BYTES);
        __DMB();
        g_ring_r = (r + 1u < SYNTH_AUDIO_RING_BLOCKS) ? (r + 1u) : 0u;
    } else {
        ++g_synth_audio_underruns;
    }
    audio_clean(dst, SYNTH_AUDIO_BLOCK_BYTES);
}

/* 主循环调用：环里还有空位就渲染一块。一次只渲染一块（~1-3 ms 的重活），
   保证每轮循环都能回去处理 USB/串口。 */
void synth_port_audio_pump(void) {
    const uint32_t w = g_ring_w;
    const uint32_t next = (w + 1u < SYNTH_AUDIO_RING_BLOCKS) ? (w + 1u) : 0u;
    if (next == g_ring_r) return;                     /* 环满：主循环领先了 */
    synth_render_i32(g_ring[w], SYNTH_PORT_BLOCK_FRAMES);
    __DMB();                                          /* 数据先落地，再发布写指针 */
    g_ring_w = next;
}

int synth_port_audio_start(void) {
    /* 预填 3 块，再把前两块搬进 DMA 的两个半缓冲，然后开循环 DMA */
    for (uint32_t i = 0; i < 3u; ++i) synth_port_audio_pump();

    for (uint32_t h = 0; h < 2u; ++h) {
        int32_t *dst = &g_dma_buf[h * SYNTH_AUDIO_BLOCK_WORDS];
        const uint32_t r = g_ring_r;
        memcpy(dst, g_ring[r], SYNTH_AUDIO_BLOCK_BYTES);
        g_ring_r = (r + 1u < SYNTH_AUDIO_RING_BLOCKS) ? (r + 1u) : 0u;
        audio_clean(dst, SYNTH_AUDIO_BLOCK_BYTES);
    }

    const uint16_t words = (uint16_t)(2u * SYNTH_AUDIO_BLOCK_WORDS);
    if (HAL_SAI_Transmit_DMA(&SYNTH_SAI_HANDLE, (uint8_t *)g_dma_buf, words) != HAL_OK) {
        return -1;
    }
    dbg_log("sai", "audio DMA started (%u words/半缓冲, 预渲染 %u 块)",
            (unsigned)SYNTH_AUDIO_BLOCK_WORDS, (unsigned)SYNTH_AUDIO_RING_BLOCKS);
    return 0;
}

void synth_port_audio_stop(void) {
    (void)HAL_SAI_DMAStop(&SYNTH_SAI_HANDLE);
}

/* ---- HAL 回调：只搬数据，不渲染 ---- */
void HAL_SAI_TxHalfCpltCallback(SAI_HandleTypeDef *hsai) {
    if (hsai == &SYNTH_SAI_HANDLE) {
        g_synth_audio_last_half = 0u;
        ++g_synth_audio_isr_depth;
        audio_serve_half(&g_dma_buf[0]);
        --g_synth_audio_isr_depth;
    }
}

void HAL_SAI_TxCpltCallback(SAI_HandleTypeDef *hsai) {
    if (hsai == &SYNTH_SAI_HANDLE) {
        g_synth_audio_last_half = 1u;
        ++g_synth_audio_isr_depth;
        audio_serve_half(&g_dma_buf[SYNTH_AUDIO_BLOCK_WORDS]);
        --g_synth_audio_isr_depth;
    }
}

void HAL_SAI_ErrorCallback(SAI_HandleTypeDef *hsai) {
    if (hsai == &SYNTH_SAI_HANDLE) {
        ++g_synth_audio_xruns;
        /* 从音频中断里调：dbg_log 只往环形缓冲里塞，不阻塞（见 debug_uart.h） */
        dbg_log("sai", "XRUN #%lu err=0x%08lX half=%ld", (unsigned long)g_synth_audio_xruns,
                (unsigned long)hsai->ErrorCode, (long)g_synth_audio_last_half);
    }
}

#endif /* SYNTH_SAI_HANDLE */
