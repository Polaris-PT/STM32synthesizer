/* SAI3 Block B + DMA 循环输出。半传输/传输完成中断里直接渲染整块。 */
#include "synth_port_config.h"
#include "synth/synth.h"

#if defined(SYNTH_SAI_HANDLE)

/* 2 个半缓冲 × 128 帧 × 2 声道，int32（24bit 左对齐） */
static int32_t SYNTH_PORT_AUDIO_RAM g_audio_buf[SYNTH_PORT_BLOCK_FRAMES * 2u * SYNTH_PORT_WORDS_PER_FRAME];

volatile uint32_t g_synth_audio_xruns = 0u;
volatile uint32_t g_synth_audio_last_half = 0xFFFFFFFFu;

/* 启动：先填满两个半缓冲，再开循环 DMA。
   Size 语义 = DMA 传输的“数据项”数（SAI 的 DR 是 32bit，一个立体声帧 = 2 项）。 */
int synth_port_audio_start(void) {
    synth_render_i32(&g_audio_buf[0], SYNTH_PORT_BLOCK_FRAMES);
    synth_render_i32(&g_audio_buf[SYNTH_PORT_BLOCK_FRAMES * SYNTH_PORT_WORDS_PER_FRAME],
                     SYNTH_PORT_BLOCK_FRAMES);

    const uint16_t words = (uint16_t)(SYNTH_PORT_BLOCK_FRAMES * 2u * SYNTH_PORT_WORDS_PER_FRAME);
    if (HAL_SAI_Transmit_DMA(&SYNTH_SAI_HANDLE, (uint8_t *)g_audio_buf, words) != HAL_OK) {
        return -1;
    }
    return 0;
}

void synth_port_audio_stop(void) {
    (void)HAL_SAI_DMAStop(&SYNTH_SAI_HANDLE);
}

/* ---- HAL 回调 ---- */
void HAL_SAI_TxHalfCpltCallback(SAI_HandleTypeDef *hsai) {
    if (hsai == &SYNTH_SAI_HANDLE) {
        g_synth_audio_last_half = 0u;
        synth_render_i32(&g_audio_buf[0], SYNTH_PORT_BLOCK_FRAMES);
    }
}

void HAL_SAI_TxCpltCallback(SAI_HandleTypeDef *hsai) {
    if (hsai == &SYNTH_SAI_HANDLE) {
        g_synth_audio_last_half = 1u;
        synth_render_i32(&g_audio_buf[SYNTH_PORT_BLOCK_FRAMES * SYNTH_PORT_WORDS_PER_FRAME],
                         SYNTH_PORT_BLOCK_FRAMES);
    }
}

void HAL_SAI_ErrorCallback(SAI_HandleTypeDef *hsai) {
    if (hsai == &SYNTH_SAI_HANDLE) {
        ++g_synth_audio_xruns;
    }
}

#endif /* SYNTH_SAI_HANDLE */
