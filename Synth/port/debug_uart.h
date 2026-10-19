/*
 * 调试串口日志（USART1：PB14 = TX、PB15 = RX、AF4、115200 8N1、无 DMA）
 *
 * 用途：把固件内部发生的事实时打出来 —— USB-MIDI 事件包、音频 DMA 欠载、主循环卡在哪个
 *   阶段、HardFault 现场、启动横幅、每秒统计行。
 *
 * 外设由 CubeMX 生成（Core/Src/usart.c 的 MX_USART1_UART_Init()，main() 里必须先调它）。
 * 接线：PB14 → USB-TTL 模块的 RX，PB15 → 模块的 TX，GND 共地（3.3V 电平，别接 5V 串口）。
 *
 * 设计：8 KB 环形缓冲（在 .bss/AXI，只有 CPU 碰它 → 无 D-Cache 一致性问题）+ 逐字节 IT
 *   发送，非阻塞、可从中断里调；缓冲满则丢弃并计数。另接管 `_write`（syscalls.c 里是 weak）
 *   → 直接 printf 也走串口。
 *
 * 开关：Synth/port/synth_port_config.h 的 SYNTH_PORT_DEBUG_UART（0 = 全空实现，不占 flash）。
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

/* 带时间戳的日志：`[  12.345] tag: ...`（毫秒来自 HAL_GetTick） */
void     dbg_log(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#else /* SYNTH_PORT_DEBUG_UART == 0：全部空实现，编译器会把调用点优化掉 */

static inline void     dbg_init(uint32_t baud) { (void)baud; }
static inline void     dbg_write(const char *s, uint32_t n) { (void)s; (void)n; }
static inline void     dbg_write_blocking(const char *s, uint32_t n) { (void)s; (void)n; }

/* 关掉时把格式化也省掉：只求值参数，不调 vsnprintf；dbg_log 连调用点一起消失，
   于是所有诊断字符串都不会再链进 flash（这就是 -DSYNTH_RELEASE=ON 走的路） */
#define dbg_printf(fmt, ...) ((void)0)
#define dbg_log(tag, ...) ((void)0)

#endif /* SYNTH_PORT_DEBUG_UART */

#endif /* SYNTH_PORT_DEBUG_UART_H */
