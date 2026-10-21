/* QSPI flash（板载 W25Q64，8 MB）底层原语：内存映射入口 + 擦/写/读。
 *
 * 为什么单独一层：内存映射模式下的"读"就是普通 load 指令（采样零拷贝播放靠它），
 * 而"写"必须走 HAL_QSPI 的间接模式（0x06/0x20/0x02 序列）—— 两者不能同时用，
 * 所以写之前要 abort 掉映射、写完再重新进映射。
 *
 * 硬件前提（CubeMX 侧）：QUADSPI Bank1 四线 PB2/PB6/PD11/PD12/PD13/PE2；
 * MPU 必须给 0x90000000 开一条可读区域（CubeMX 默认的 4 GB NO_ACCESS 会把它挡住）。
 */
#ifndef SYNTH_PORT_QSPI_FLASH_H
#define SYNTH_PORT_QSPI_FLASH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNTH_QSPI_MAP_BASE     0x90000000u   /* 内存映射窗口基址 */
#define SYNTH_QSPI_SIZE_BYTES   (8u * 1024u * 1024u)
#define SYNTH_QSPI_SECTOR_BYTES 4096u         /* 擦除粒度 */
#define SYNTH_QSPI_PAGE_BYTES   256u          /* 页编程粒度 */

/* 进内存映射（0xEB 1-4-4，dummy 4）。返回 0 = 成功、负数 = 停在第几步
   （-1..-11 与 docs/phase2-review.md 里 QSPI 自检一致）。 */
int      synth_port_qspi_enter_mmap(void);

/* 只重新进映射（比 enter_mmap 便宜：不软复位、不读 ID/QE）。间接读/擦写之后想继续用
   0x90000000 上的指针时调用；ensure 版本在已映射时直接返回 0。 */
int      synth_port_qspi_mmap_reenter(void);
int      synth_port_qspi_ensure_mmap(void);

/* 最近一次读到的 JEDEC ID（0xEF4017 = W25Q64）。读 ID 失败时为 0。 */
uint32_t synth_port_qspi_id(void);

/* 写使能（0x06）。擦/写之前必须先发。 */
int      synth_port_qspi_write_enable(void);

/* 轮询 SR1.WIP 直到清零；0 = 空闲、-1 = 超时。 */
int      synth_port_qspi_wait_wip(void);

/* 擦一个 4 KB 扇区（0x20）。内部会先 abort 内存映射。 */
int      synth_port_qspi_erase_sector(uint32_t offset);

/* 页编程（0x02）：offset 可以不对齐页边界，但一次不能跨页（调用方按 256 B 切）。 */
int      synth_port_qspi_page_program(uint32_t offset, const void *data, uint32_t len);

/* 间接读（0x03 单线，慢但不需要映射）；导入校验/映射未就绪时用。 */
int      synth_port_qspi_read(uint32_t offset, void *buf, uint32_t len);

/* 写完 flash 后调用：让 CPU 看到新数据（0x90000000 区可缓存时必须）。 */
void     synth_port_qspi_cache_invalidate(void);

#ifdef __cplusplus
}
#endif
#endif /* SYNTH_PORT_QSPI_FLASH_H */
