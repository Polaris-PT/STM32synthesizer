/* SAI3 Block B + DMA 循环输出。半传输/传输完成中断里直接渲染整块。 */
#include "synth_port_config.h"
#include "synth/synth.h"
#include "debug_uart.h"

#if defined(SYNTH_SAI_HANDLE)

/* 2 个半缓冲 × 128 帧 × 2 声道，int32（24bit 左对齐）。
   必须放 AXI SRAM（.ram_section）：DMA1/DMA2 访问不到 DTCM。
   AXI SRAM 是 Cacheable 的，所以 DMA 读之前要把刚渲染的数据写回内存（audio_clean）。 */
static int32_t SYNTH_PORT_AUDIO_RAM g_audio_buf[SYNTH_PORT_BLOCK_FRAMES * 2u * SYNTH_PORT_WORDS_PER_FRAME]
    __attribute__((aligned(32)));

/* 半个缓冲的字节数（= 128 帧 × 2 声道 × 4 字节 = 1KB，天然 32 字节对齐倍数） */
#define SYNTH_AUDIO_HALF_BYTES  ((uint32_t)sizeof(g_audio_buf) / 2u)

static inline void audio_clean(const int32_t *half_buf) {
    SCB_CleanDCache_by_Addr((uint32_t *)(uintptr_t)half_buf, (int32_t)SYNTH_AUDIO_HALF_BYTES);
}

volatile uint32_t g_synth_audio_xruns = 0u;
volatile uint32_t g_synth_audio_last_half = 0xFFFFFFFFu;
/* 存活探针：>0 = 正在音频 DMA 中断里渲染。
   这个中断是优先级 0（最高），如果渲染卡住（死循环/长时间阻塞），SysTick（优先级 15）会被屏蔽 ——
   现象就是"整块板像死机"（LED 不闪、USB 不响应），而 loop_count/systick_ticks 都停住。
   所以它是区分"卡在渲染里"和"卡在主循环里"的关键字段。 */
volatile uint32_t g_synth_audio_isr_depth = 0u;

/* 启动：先填满两个半缓冲，再开循环 DMA。
   Size 语义 = DMA 传输的“数据项”数（SAI 的 DR 是 32bit，一个立体声帧 = 2 项）。 */
int synth_port_audio_start(void) {
    synth_render_i32(&g_audio_buf[0], SYNTH_PORT_BLOCK_FRAMES);
    audio_clean(&g_audio_buf[0]);
    synth_render_i32(&g_audio_buf[SYNTH_PORT_BLOCK_FRAMES * SYNTH_PORT_WORDS_PER_FRAME],
                     SYNTH_PORT_BLOCK_FRAMES);
    audio_clean(&g_audio_buf[SYNTH_PORT_BLOCK_FRAMES * SYNTH_PORT_WORDS_PER_FRAME]);

    const uint16_t words = (uint16_t)(SYNTH_PORT_BLOCK_FRAMES * 2u * SYNTH_PORT_WORDS_PER_FRAME);
    if (HAL_SAI_Transmit_DMA(&SYNTH_SAI_HANDLE, (uint8_t *)g_audio_buf, words) != HAL_OK) {
        return -1;
    }
    dbg_log("sai", "audio DMA started (%u words, %lu Hz)", (unsigned)words, (unsigned long)48000u);
    return 0;
}

void synth_port_audio_stop(void) {
    (void)HAL_SAI_DMAStop(&SYNTH_SAI_HANDLE);
}

/* ---- HAL 回调 ---- */
void HAL_SAI_TxHalfCpltCallback(SAI_HandleTypeDef *hsai) {
    if (hsai == &SYNTH_SAI_HANDLE) {
        g_synth_audio_last_half = 0u;
        ++g_synth_audio_isr_depth;          /* 卡死时这里会一直是 1（见文件顶部说明） */
        synth_render_i32(&g_audio_buf[0], SYNTH_PORT_BLOCK_FRAMES);
        audio_clean(&g_audio_buf[0]);       /* D-Cache 写回，否则 DMA 送出的是旧数据 */
        --g_synth_audio_isr_depth;
    }
}

void HAL_SAI_TxCpltCallback(SAI_HandleTypeDef *hsai) {
    if (hsai == &SYNTH_SAI_HANDLE) {
        g_synth_audio_last_half = 1u;
        ++g_synth_audio_isr_depth;
        synth_render_i32(&g_audio_buf[SYNTH_PORT_BLOCK_FRAMES * SYNTH_PORT_WORDS_PER_FRAME],
                         SYNTH_PORT_BLOCK_FRAMES);
        audio_clean(&g_audio_buf[SYNTH_PORT_BLOCK_FRAMES * SYNTH_PORT_WORDS_PER_FRAME]);
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
