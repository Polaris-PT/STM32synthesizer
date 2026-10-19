/*
 * 调试串口（USART1，PA9 = TX / PA10 = RX，AF7，默认 115200 8N1）
 *
 * 用途：把固件内部发生的事实时打出来，最常用于抓
 *   - USB-MIDI 收到的每个 4 字节事件包（判断 note-off 到底有没有到）
 *   - 音频 DMA 的 underrun / 渲染超时
 *   - 主循环卡在哪个阶段、进了几次 HardFault
 *
 * 硬件：走 **CubeMX 生成的 USART1**（PB14 = TX、PB15 = RX、AF4、115200 8N1、无 DMA），
 *   外设初始化在 Core/Src/usart.c 的 MX_USART1_UART_Init()（main() 里要先调它）。
 *   接线：PB14 → USB-TTL 模块的 RX；PB15 → 模块的 TX；GND 共地（3.3V 电平，别接 5V 串口）。
 *
 * 设计要点：
 *   - 发送走 8 KB 环形缓冲 + 逐字节 IT 发送，**不阻塞任何调用者**（可以从中断里调）
 *   - 环形缓冲在 .bss（AXI SRAM），只有 CPU 碰它 → 不涉及 D-Cache 一致性
 *   - 缓冲满时丢弃并计数（g_dbg_dropped_bytes），不会拖慢实时路径
 *   - 还接管了 `_write`（syscalls.c 里的是 weak）→ 直接 printf 也会走串口
 *
 * 开关：Synth/port/synth_port_config.h 里的 SYNTH_PORT_DEBUG_UART（0 = 全空实现，不占 flash）
 */

#ifndef SYNTH_PORT_DEBUG_UART_H
#define SYNTH_PORT_DEBUG_UART_H

#include <stdint.h>
#include "synth_port_config.h"

#if SYNTH_PORT_DEBUG_UART

/* 初始化（必须在 SystemClock_Config() 之后调，波特率按 PCLK2 计算） */
void     dbg_init(uint32_t baud);

/* 非阻塞写：进环形缓冲，满则丢；可从任意上下文（含中断）调用 */
void     dbg_write(const char *s, uint32_t n);

/* 阻塞写：直接轮询 TXE 一个字节一个字节发。只给 HardFault 等"发完就死"的场景用 */
void     dbg_write_blocking(const char *s, uint32_t n);

/* 格式化（内部 vsnprintf，单条上限 255 字节；超出会截断） */
void     dbg_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* 十六进制转储，形如 "01 90 3C 40" */
void     dbg_hex(const uint8_t *p, uint32_t n);

/* 环形缓冲的丢弃字节数（读走清零） */
uint32_t dbg_take_dropped(void);

#else /* SYNTH_PORT_DEBUG_UART == 0：全部空实现，编译器会把调用点优化掉 */

static inline void     dbg_init(uint32_t baud) { (void)baud; }
static inline void     dbg_write(const char *s, uint32_t n) { (void)s; (void)n; }
static inline void     dbg_write_blocking(const char *s, uint32_t n) { (void)s; (void)n; }
static inline void     dbg_hex(const uint8_t *p, uint32_t n) { (void)p; (void)n; }
static inline uint32_t dbg_take_dropped(void) { return 0u; }

/* 关掉时把格式化也省掉：只求值参数，不调 vsnprintf */
#define dbg_printf(fmt, ...) ((void)0)

#endif /* SYNTH_PORT_DEBUG_UART */

/* 带时间戳的日志：`[  12.345] tag: ...`（毫秒来自 HAL_GetTick） */
void dbg_log(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif /* SYNTH_PORT_DEBUG_UART_H */
