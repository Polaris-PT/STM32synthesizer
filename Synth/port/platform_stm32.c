/* 平台初始化：D-Cache、浮点 FTZ、DWT 周期计数、USB 48MHz 时钟、平台钩子安装。 */
#include "synth_port_config.h"
#include "synth/synth.h"
#include "debug_uart.h"

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

/* USB 内核时钟配置的返回值（0 = HAL_OK）。诊断用：main.c 抄进 g_app_status.usb_clk_rc。
   放在 #if 之外 —— USB 关掉时 main.c 仍会读它。 */
volatile uint32_t g_synth_usb_clk_rc = 0xFFFFFFFFu;
/* 存活探针（用 g_app_status 判死活，见 Core/Src/main.c 顶部"诊断代码清单"）：
   systick_ticks 由 SysTick 自增；isr_depth 用来判断"是不是卡死在中断里"。
   放在 #if 之外是为了 USB 关掉时 main.c 也能链接。 */
volatile uint32_t g_synth_systick_ticks = 0u;
volatile uint32_t g_synth_usb_isr_depth = 0u;

#if SYNTH_PORT_USB_ENABLE
/* USB OTG_FS 硬件准备（TinyUSB 的 dwc2 驱动只碰寄存器，GPIO/时钟/中断必须板级配好）。
   中断优先级 5：低于音频 DMA（0），避免 USB 抢音频中断导致爆音。 */
static void usb_hw_init(void) {
    /* ⚠ 漏了这一行 = 主机侧完全看不到设备。H7 的 FS PHY 由 VDD33USB 供电，内部 USB 稳压器
       使能时 VDD33USB 电平检测器也必须打开（stm32h7xx_hal_pwr_ex.c:1598-1602）；不打开时
       DP 上拉不生效，主机永远等不到连接。CubeMX 给同一块板生成的 HAL_PCD_MspInit() 第一句
       就是它。TinyUSB 不管 PWR：dwc2_phy_init() 只设 GCCFG.PWRDWN、dwc2_clock_init() 是空的。 */
    HAL_PWREx_EnableUSBVoltageDetector();

    /* 内核时钟 = PLL1Q = 48.000 MHz（main.c 的 SystemClock_Config 里 PLL1Q = 20） */
    RCC_PeriphCLKInitTypeDef periph = {0};
    periph.PeriphClockSelection = RCC_PERIPHCLK_USB;
    periph.UsbClockSelection    = RCC_USBCLKSOURCE_PLL;   /* PLL1Q，不是 PLL3 */
    g_synth_usb_clk_rc = (uint32_t)HAL_RCCEx_PeriphCLKConfig(&periph);

    __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin       = GPIO_PIN_11 | GPIO_PIN_12;           /* DM / DP */
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF10_OTG1_FS;
    HAL_GPIO_Init(GPIOA, &gpio);

    __HAL_RCC_USB_OTG_FS_CLK_ENABLE();

    HAL_NVIC_SetPriority(OTG_FS_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(OTG_FS_IRQn);

    dbg_log("usb", "hw init: clkcfg_rc=%ld", (long)g_synth_usb_clk_rc);
}

#endif /* SYNTH_PORT_USB_ENABLE */

void synth_port_init(void) {
    dbg_log("init", "synth_port_init: D-Cache / FPU(FTZ) / DWT");

    /* D-Cache：CubeMX 默认只开 I-Cache，DSP 需要 D-Cache */
    SCB_EnableDCache();

    /* 浮点：非规格化数清零（避免 DSP 里出现 denormal 拖慢） */
    FPU->FPDSCR |= FPU_FPDSCR_FZ_Msk;

    /* DWT 周期计数器 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

#if SYNTH_PORT_USB_ENABLE
    usb_hw_init();
#endif

    synth_set_platform_hooks(&g_hooks);
}
