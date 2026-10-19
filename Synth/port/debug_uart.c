/*
 * 调试串口实现（走 CubeMX 生成的 USART1：PB14 = TX、PB15 = RX、AF4、115200 8N1、无 DMA）
 *
 * 分工：
 *   - 外设初始化 / 引脚 / 时钟：CubeMX 生成的 Core/Src/usart.c（MX_USART1_UART_Init()）
 *   - 本文件只做「环形缓冲 + 逐字节 IT 发送 + printf 重定向 + 日志封装」
 *
 * 为什么不用 DMA（CubeMX 里也没开）：
 *   115200 下每字节约 87 µs，逐字节中断发送的额外开销只有每字节 ~2 µs；日志是诊断通道，
 *   不需要吞吐，环形缓冲已经把"慢"挡在实时路径之外（调用者只 memcpy 就返回）。
 *   省下一个 DMA 流，也避免 AXI SRAM 上 DMA 的 D-Cache 一致性问题。
 *
 * ⚠ 中断优先级：这里自己在 dbg_init() 里使能 USART1 中断（优先级 9，低于音频 DMA 的 0
 *   和 USB 的 5）。若以后在 CubeMX 里给 USART1 勾了 NVIC，CubeMX 会在 stm32h7xx_it.c 里
 *   生成强符号的 USART1_IRQHandler；本文件的同名函数声明为 weak，会自动让位（不会重复定义）。
 */

#include "debug_uart.h"

#if SYNTH_PORT_DEBUG_UART

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "stm32h7xx_hal.h"
#include "usart.h"          /* huart1 / MX_USART1_UART_Init()（CubeMX 生成） */

#define DBG_RING_SIZE      8192u   /* 2 的幂才不用取模 */
#define DBG_RING_MASK      (DBG_RING_SIZE - 1u)
#define DBG_LOG_LINE_MAX   256u
#define DBG_IRQ_PRIORITY   9u

/* 计数放 .ram_section：软复位不丢，方便用 openocd 读（tools/status.ps1） */
volatile uint32_t g_dbg_sent_bytes    __attribute__((section(".ram_section"))) = 0u;
volatile uint32_t g_dbg_dropped_bytes __attribute__((section(".ram_section"))) = 0u;

static uint8_t           s_ring[DBG_RING_SIZE];
static volatile uint32_t s_head;        /* 生产者写 */
static volatile uint32_t s_tail;        /* 消费者（发送完成中断里推进）写 */
static uint32_t          s_dropped;
static volatile uint8_t  s_tx_byte;     /* HAL_UART_Transmit_IT 的源缓冲（必须常驻） */
static volatile uint8_t  s_tx_busy;     /* 1 = 已有一字节在发 */

static void dbg_irq_mask(uint32_t *primask)
{
  *primask = __get_PRIMASK();
  __disable_irq();
}

static void dbg_irq_unmask(uint32_t primask)
{
  if (primask == 0u) {
    __enable_irq();
  }
}

/* 若发送空闲且有数据，就取一个字节启动 IT 传输。可从主循环/中断/上一字节的完成回调里调。 */
static void dbg_kick(void)
{
  uint32_t primask;
  dbg_irq_mask(&primask);

  if (s_tx_busy != 0u) {
    dbg_irq_unmask(primask);
    return;
  }

  const uint32_t tail = s_tail;
  if (tail == s_head) {                       /* 空 */
    dbg_irq_unmask(primask);
    return;
  }

  s_tx_byte = s_ring[tail & DBG_RING_MASK];
  s_tail = tail + 1u;
  s_tx_busy = 1u;
  dbg_irq_unmask(primask);

  if (HAL_UART_Transmit_IT(&huart1, (uint8_t *)(uintptr_t)&s_tx_byte, 1u) != HAL_OK) {
    s_tx_busy = 0u;                           /* 启动失败（例如外设没初始化）：不卡死调用者 */
  }
}

void dbg_init(uint32_t baud)
{
  (void)baud;                                 /* 波特率由 CubeMX 的 usart.c 决定 */

  s_head = 0u;
  s_tail = 0u;
  s_tx_busy = 0u;
  s_dropped = 0u;

  /* 这两个计数在 .ram_section（链接脚本里是 NOLOAD）→ 上电后是随机值，必须自己清零；
     它们故意放在 .ram_section 是为了软复位后不丢（HardFault 后能看出崩之前发了多少日志）。 */
  g_dbg_sent_bytes = 0u;
  g_dbg_dropped_bytes = 0u;

  /* CubeMX 没勾 USART1 的 NVIC（it.c 里没有 handler），这里自己使能；
     本文件的 USART1_IRQHandler 是 weak，若以后 CubeMX 生成了强符号会自动接管。 */
  HAL_NVIC_SetPriority(USART1_IRQn, DBG_IRQ_PRIORITY, 0u);
  HAL_NVIC_EnableIRQ(USART1_IRQn);
}

void dbg_write(const char *s, uint32_t n)
{
  if ((s == NULL) || (n == 0u)) {
    return;
  }

  uint32_t primask;
  dbg_irq_mask(&primask);

  uint32_t head  = s_head;
  uint32_t tail  = s_tail;
  uint32_t free  = DBG_RING_SIZE - ((head - tail) & DBG_RING_MASK) - 1u;

  if (n > free) {
    s_dropped  += (n - free);
    g_dbg_dropped_bytes = s_dropped;
    n = free;                                 /* 只写能放下的部分 */
  }

  for (uint32_t i = 0u; i < n; ++i) {
    s_ring[(head + i) & DBG_RING_MASK] = (uint8_t)s[i];
  }
  s_head = head + n;
  g_dbg_sent_bytes += n;

  dbg_irq_unmask(primask);

  dbg_kick();
}

void dbg_write_blocking(const char *s, uint32_t n)
{
  if ((s == NULL) || (n == 0u)) {
    return;
  }
  for (uint32_t i = 0u; i < n; ++i) {
    /* 直接轮询 TXE：不依赖 HAL 状态机、不依赖 SysTick（HardFault 里 SysTick 可能已停） */
    while ((USART1->ISR & USART_ISR_TXE_TXFNF) == 0u) {
    }
    USART1->TDR = (uint8_t)s[i];
    ++g_dbg_sent_bytes;
  }
  while ((USART1->ISR & USART_ISR_TC) == 0u) {   /* 等最后一个字节真正移出 */
  }
}

void dbg_printf(const char *fmt, ...)
{
  char    line[DBG_LOG_LINE_MAX];
  va_list ap;

  va_start(ap, fmt);
  const int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);

  if (n <= 0) {
    return;
  }
  dbg_write(line, ((uint32_t)n < sizeof(line)) ? (uint32_t)n : (uint32_t)(sizeof(line) - 1u));
}

/* ── 日志封装 ─────────────────────────────────────────────────────────────── */

void dbg_log(const char *tag, const char *fmt, ...)
{
  char    buf[DBG_LOG_LINE_MAX];
  va_list ap;

  va_start(ap, fmt);
  const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  const uint32_t ms = HAL_GetTick();
  dbg_printf("[%5lu.%03lu] %-6s ", (unsigned long)(ms / 1000u), (unsigned long)(ms % 1000u),
             (tag != NULL) ? tag : "-");
  if (n > 0) {
    dbg_write(buf, ((uint32_t)n < sizeof(buf)) ? (uint32_t)n : (uint32_t)(sizeof(buf) - 1u));
  }
  dbg_printf("\r\n");
}

/* ── HAL 回调 / 中断 ──────────────────────────────────────────────────────── */

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart == &huart1) {
    s_tx_busy = 0u;
    dbg_kick();                               /* 紧接着发下一个字节 */
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart == &huart1) {
    s_tx_busy = 0u;                           /* 出错就放掉忙标志，别把日志通道卡死 */
  }
}

/* ⚠ 必须是**强符号**：startup_stm32h750xx.s 里已经给 USART1_IRQHandler 声明了 weak 别名
 *   （指向 Default_Handler 的无限循环）。如果这里也写 weak，链接器会挑 startup 那份，
 *   中断一来就跳进 Infinite_Loop —— 表现为「烧进去就不动了」（我踩过：板子卡在 IRQ 37）。
 *   以后若在 CubeMX 里给 USART1 勾了 NVIC，it.c 会生成强符号同名函数 → **把本函数删掉**。 */
void USART1_IRQHandler(void)
{
  HAL_UART_IRQHandler(&huart1);
}

/* 接管 printf/puts：syscalls.c 里的 _write 是 weak，这里强定义覆盖它 */
int _write(int file, char *ptr, int len)
{
  (void)file;
  if ((ptr != NULL) && (len > 0)) {
    dbg_write(ptr, (uint32_t)len);
  }
  return len;
}

#endif /* SYNTH_PORT_DEBUG_UART —— 关闭时本文件编译为空：
          dbg_log 在 debug_uart.h 里是空宏，调用点连同格式字符串一起消失 */
