/* 平台初始化：D-Cache、浮点 FTZ、DWT 周期计数、USB 48MHz 时钟、平台钩子安装。 */
#include "synth_port_config.h"
#include "synth/synth.h"

/* ---- 周期计数（DWT->CYCCNT） ---- */
uint32_t synth_port_cycles_now(void) {
    return DWT->CYCCNT;
}

uint32_t synth_port_cpu_hz(void) {
    return SystemCoreClock;
}

static const synth_platform_hooks_t g_hooks = {
    &synth_port_cycles_now,
    &synth_port_cpu_hz,
};

void synth_port_init(void) {
    /* D-Cache：CubeMX 默认只开 I-Cache，DSP 需要 D-Cache */
    SCB_EnableDCache();

    /* 浮点：非规格化数清零（避免 DSP 里出现 denormal 拖慢） */
    FPU->FPDSCR |= FPU_FPDSCR_FZ_Msk;

    /* DWT 周期计数器 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    /* USB 内核时钟 = PLL1Q 48.000MHz（CubeMX 时钟树里配 PLL1Q=48MHz） */
    RCC_PeriphCLKInitTypeDef periph = {0};
    periph.PeriphClockSelection = RCC_PERIPHCLK_USB;
    periph.UsbClockSelection    = RCC_USBCLKSOURCE_PLL;
    (void)HAL_RCCEx_PeriphCLKConfig(&periph);

    synth_set_platform_hooks(&g_hooks);
}

/* 供 CubeMX 的 Error_Handler 之外做一次硬复位（可选） */
void synth_port_hard_fault_hook(void) {
    NVIC_SystemReset();
}
